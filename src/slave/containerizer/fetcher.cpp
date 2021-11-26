// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "slave/containerizer/fetcher.hpp"

#include <process/async.hpp>
#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/net.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/uri.hpp>
#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <stout/os/exists.hpp>
#include <stout/os/find.hpp>
#include <stout/os/killtree.hpp>
#include <stout/os/realpath.hpp>
#include <stout/os/read.hpp>
#include <stout/os/rmdir.hpp>

#include "hdfs/hdfs.hpp"

#include "common/status_utils.hpp"

#include "slave/containerizer/fetcher_process.hpp"

using std::list;
using std::map;
using std::shared_ptr;
using std::string;
using std::transform;
using std::vector;

using strings::startsWith;

using mesos::fetcher::FetcherInfo;

using process::async;

using process::Failure;
using process::Future;
using process::Owned;
using process::Subprocess;

namespace mesos {
namespace internal {
namespace slave {

static const string CACHE_FILE_NAME_PREFIX = "c";


Fetcher::Fetcher(const Flags& flags) : process(new FetcherProcess(flags))
{
  if (os::exists(flags.fetcher_cache_dir)) {
    Try<Nothing> rmdir = os::rmdir(flags.fetcher_cache_dir, true);
    CHECK_SOME(rmdir)
      << "Could not delete fetcher cache directory '"
      << flags.fetcher_cache_dir << "': " + rmdir.error();
  }

  spawn(process.get());
}


Fetcher::Fetcher(const process::Owned<FetcherProcess>& process)
  : process(process)
{
  spawn(process.get());
}


Fetcher::~Fetcher()
{
  terminate(process.get());
  process::wait(process.get());
}


Try<string> Fetcher::basename(const string& uri)
{
  // TODO(bernd-mesos): full URI parsing, then move this to stout.
  // There is a bug (or is it a feature?) in the original fetcher
  // code without caching that remains in effect here. URIs are
  // treated like file paths, looking for occurrences of "/",
  // but ignoring other separators that can show up
  // (e.g. "?", "=" in HTTP URLs).

  if (uri.find_first_of('\\') != string::npos ||
      uri.find_first_of('\'') != string::npos ||
      uri.find_first_of('\0') != string::npos) {
      return Error("Illegal characters in URI");
  }

  size_t index = uri.find("://");
  if (index != string::npos && 1 < index) {
    // URI starts with protocol specifier, e.g., http://, https://,
    // ftp://, ftps://, hdfs://, hftp://, s3://, s3n://.

    string path = uri.substr(index + 3);
    if (!strings::contains(path, "/") || path.size() <= path.find('/') + 1) {
      return Error("Malformed URI (missing path): " + uri);
    }

    return path.substr(path.find_last_of('/') + 1);
  }
  return Path(uri).basename();
}


Try<Nothing> Fetcher::validateUri(const string& uri)
{
  Try<string> result = basename(uri);
  if (result.isError()) {
    return Error(result.error());
  }

  return Nothing();
}


Try<Nothing> Fetcher::validateOutputFile(const string& path)
{
  Try<string> result = Path(path).basename();
  if (result.isError()) {
    return Error(result.error());
  }

  if (path.size() == 0) {
    return Error("URI output file path is empty");
  }

  // TODO(mrbrowning): Check that the filename's directory component is
  // actually a subdirectory of the sandbox, not just relative to it.
  if (strings::startsWith(path, '/')) {
    return Error("URI output file must be within the sandbox directory");
  }

  return Nothing();
}


static Try<Nothing> validateUris(const CommandInfo& commandInfo)
{
  foreach (const CommandInfo::URI& uri, commandInfo.uris()) {
    Try<Nothing> uriValidation = Fetcher::validateUri(uri.value());
    if (uriValidation.isError()) {
      return Error(uriValidation.error());
    }

    if (uri.has_output_file()) {
      Try<Nothing> outputFileValidation =
        Fetcher::validateOutputFile(uri.output_file());
      if (outputFileValidation.isError()) {
        return Error(outputFileValidation.error());
      }
    }
  }

  return Nothing();
}


Result<string> Fetcher::uriToLocalPath(
    const string& uri,
    const Option<string>& frameworksHome)
{
  const bool fileUri = strings::startsWith(uri, uri::FILE_PREFIX);

  if (!fileUri && strings::contains(uri, "://")) {
    return None();
  }

  // TODO(andschwa): Fix `path::from_uri` to remove hostname component, which it
  // currently does not do, so we remove `localhost` manually here.
  string path =
    strings::remove(path::from_uri(uri), "localhost", strings::PREFIX);

  if (!path::is_absolute(path)) {
    if (fileUri) {
      return Error("File URI only supports absolute paths");
    }

    if (frameworksHome.isNone() || frameworksHome->empty()) {
      return Error("A relative path was passed for the resource but the "
                   "Mesos framework home was not specified. "
                   "Please either provide this config option "
                   "or avoid using a relative path");
    } else {
      path = path::join(frameworksHome.get(), path);
      LOG(INFO) << "Prepended Mesos frameworks home to relative path, "
                << "making it: '" << path << "'";
    }
  }

  return path;
}


bool Fetcher::isNetUri(const string& uri)
{
  return strings::startsWith(uri, "http://")  ||
         strings::startsWith(uri, "https://") ||
         strings::startsWith(uri, "ftp://")   ||
         strings::startsWith(uri, "ftps://");
}


Future<Nothing> Fetcher::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& sandboxDirectory,
    const Option<string>& user)
{
  return dispatch(process.get(),
                  &FetcherProcess::fetch,
                  containerId,
                  commandInfo,
                  sandboxDirectory,
                  user);
}


void Fetcher::kill(const ContainerID& containerId)
{
  dispatch(process.get(), &FetcherProcess::kill, containerId);
}


FetcherProcess::Metrics::Metrics(FetcherProcess *fetcher)
  : task_fetches_succeeded("containerizer/fetcher/task_fetches_succeeded"),
    task_fetches_failed("containerizer/fetcher/task_fetches_failed"),
    cache_size_total_bytes(
        "containerizer/fetcher/cache_size_total_bytes",
        [=]() {
          // This value is safe to read while it is concurrently updated.
          return static_cast<double>(fetcher->cache.totalSpace().bytes());
        }),
    cache_size_used_bytes(
        "containerizer/fetcher/cache_size_used_bytes",
        [=]() {
          // This value is safe to read while it is concurrently updated.
          return static_cast<double>(fetcher->cache.usedSpace().bytes());
        })
{
  process::metrics::add(task_fetches_succeeded);
  process::metrics::add(task_fetches_failed);
  process::metrics::add(cache_size_total_bytes);
  process::metrics::add(cache_size_used_bytes);
}


FetcherProcess::Metrics::~Metrics()
{
  process::metrics::remove(task_fetches_succeeded);
  process::metrics::remove(task_fetches_failed);

  // Wait for the metrics to be removed before we allow the destructor
  // to complete.
  await(
      process::metrics::remove(cache_size_total_bytes),
      process::metrics::remove(cache_size_used_bytes)).await();
}


FetcherProcess::FetcherProcess(const Flags& _flags)
    : ProcessBase(process::ID::generate("fetcher")),
      metrics(this),
      flags(_flags),
      cache(_flags.fetcher_cache_size)
{
}


FetcherProcess::~FetcherProcess()
{
  foreachkey (const ContainerID& containerId, subprocessPids) {
    kill(containerId);
  }
}


// Find out how large a potential download from the given URI is.
static Try<Bytes> fetchSize(
    const string& uri,
    const Option<string>& frameworksHome)
{
  VLOG(1) << "Fetching size for URI: " << uri;

  Result<string> path = Fetcher::uriToLocalPath(uri, frameworksHome);
  if (path.isError()) {
    return Error(path.error());
  }
  if (path.isSome()) {
    Try<Bytes> size = os::stat::size(
        path.get(), os::stat::FollowSymlink::FOLLOW_SYMLINK);
    if (size.isError()) {
      return Error("Could not determine file size for: '" + path.get() +
                     "', error: " + size.error());
    }
    return size.get();
  }

  if (Fetcher::isNetUri(uri)) {
    Try<Bytes> size = net::contentLength(uri);
    if (size.isError()) {
      return Error(size.error());
    }
    if (size.get() == 0) {
      return Error("URI reported content-length 0: " + uri);
    }

    return size.get();
  }

  // TODO(hausdorff): (MESOS-5460) Explore adding support for fetching from
  // HDFS.
#ifndef __WINDOWS__
  Try<Owned<HDFS>> hdfs = HDFS::create();
  if (hdfs.isError()) {
    return Error("Failed to create HDFS client: " + hdfs.error());
  }

  Future<Bytes> size = hdfs.get()->du(uri);
  size.await();

  if (!size.isReady()) {
    return Error("Hadoop client could not determine size: " +
                 (size.isFailed() ? size.failure() : "discarded"));
  }

  return size.get();
#else
  return Error("Windows currently does not support fetching files from HDFS");
#endif // __WINDOWS__
}


Future<Nothing> FetcherProcess::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& sandboxDirectory,
    const Option<string>& user)
{
  VLOG(1) << "Starting to fetch URIs for container: " << containerId
          << ", directory: " << sandboxDirectory;

  Try<Nothing> validated = validateUris(commandInfo);
  if (validated.isError()) {
    ++metrics.task_fetches_failed;
    return Failure("Could not fetch: " + validated.error());
  }

  Option<string> commandUser = user;
  if (commandInfo.has_user()) {
    commandUser = commandInfo.user();
  }

  string cacheDirectory = flags.fetcher_cache_dir;
  if (commandUser.isSome()) {
    // Segregating per-user cache directories.
    cacheDirectory = path::join(cacheDirectory, commandUser.get());
  }

  // For each URI we determine if we should use the cache and if so we
  // try and either get the cache entry or create a cache entry. If
  // we're getting the cache entry then we might need to wait for that
  // cache entry to be downloaded. If we're creating a new cache entry
  // then we need to properly reserve the cache space (and perform any
  // evictions). Thus, there are three possibilities for each URI:
  //
  //   (1) We are not using the cache.
  //   (2) We are using the cache but need to wait for an entry to be
  //       downloaded.
  //   (3) We are using the cache and need to create a new entry.
  //
  // We capture whether or not we're using the cache using an Option
  // as a value in a map, i.e., if we are not trying to use the cache
  // as in (1) above then the Option is None otherwise as in (2) and
  // (3) the Option is Some. And to capture the asynchronous nature of
  // both (2) and (3) that Option holds a Future to the actual cache
  // entry.
  hashmap<CommandInfo::URI, Option<Future<shared_ptr<Cache::Entry>>>> entries;

  // When we create new entries, we need to track whether we already have
  // a entry for the corresponding URI value. This handles the case where
  // multiple entries have the same URI value (but hash differently because
  // they differ in other fields). If we see the same URI value multiple
  // times, then we simply add references the initial entry.
  hashmap<string, shared_ptr<Cache::Entry>> newEntries;

  foreach (const CommandInfo::URI& uri, commandInfo.uris()) {
    if (!uri.cache()) {
      entries[uri] = None();
      continue;
    }

    if (newEntries.contains(uri.value())) {
      newEntries[uri.value()]->reference();
      entries[uri] = newEntries.at(uri.value());
      continue;
    }

    // Check if this is already in the cache (but not necessarily
    // downloaded).
    const Option<shared_ptr<Cache::Entry>> entry =
      cache.get(commandUser, uri.value());

    if (entry.isSome()) {
      entry.get()->reference();

      // Wait for the URI to be downloaded into the cache (or fail)
      entries[uri] = entry.get()->completion()
        .then(defer(self(), [=]() {
          return Future<shared_ptr<Cache::Entry>>(entry.get());
        }));
    } else {
      shared_ptr<Cache::Entry> newEntry =
        cache.create(cacheDirectory, commandUser, uri);

      newEntries.put(uri.value(), newEntry);
      newEntry->reference();

      entries[uri] =
        async([=]() {
          return fetchSize(uri.value(), flags.frameworks_home);
        })
        .then(defer(self(), [=](const Try<Bytes>& requestedSpace) {
          return reserveCacheSpace(requestedSpace, newEntry);
        }));
    }
  }

  // NOTE: We explicitly call the continuation '_fetch' even though it
  // looks like we could easily inline it here because we want to be
  // able to mock the function for testing! Don't remove this!
  return _fetch(entries,
                containerId,
                sandboxDirectory,
                cacheDirectory,
                commandUser);
}


Future<Nothing> FetcherProcess::_fetch(
    const hashmap<CommandInfo::URI, Option<Future<shared_ptr<Cache::Entry>>>>&
      entries,
    const ContainerID& containerId,
    const string& sandboxDirectory,
    const string& cacheDirectory,
    const Option<string>& user)
{
  // Get out all of the futures we need to wait for so we can wait on
  // them together via 'await'.
  vector<Future<shared_ptr<Cache::Entry>>> futures;

  foreachvalue (const Option<Future<shared_ptr<Cache::Entry>>>& entry,
                entries) {
    if (entry.isSome()) {
      futures.push_back(entry.get());
    }
  }

  return await(futures)
    .then(defer(self(), [=]() {
      // For each URI, if there is a potential cache entry and waiting
      // for its associated future was successful, extract the entry
      // from the future and store it in 'result'. Otherwise we assume
      // we are not using or cannot use the cache for this URI.
      hashmap<CommandInfo::URI, Option<shared_ptr<Cache::Entry>>> result;

      foreachpair (const CommandInfo::URI& uri,
                   const Option<Future<shared_ptr<Cache::Entry>>>& entry,
                   entries) {
        if (entry.isSome()) {
          if (entry->isReady()) {
            result[uri] = entry->get();
          } else {
            LOG(WARNING)
              << "Reverting to fetching directly into the sandbox for '"
              << uri.value()
              << "', due to failure to fetch through the cache, "
              << "with error: " << entry->failure();

            result[uri] = None();
          }
        } else {
          // No entry means bypassing the cache.
          result[uri] = None();
        }
      }

      // NOTE: While we could inline '__fetch' we've explicitly kept
      // it as a separate function to minimize complexity. Like with
      // '_fetch', this also enables this phase of the fetcher cache
      // to easily be mocked for testing!
      return __fetch(result,
                     containerId,
                     sandboxDirectory,
                     cacheDirectory,
                     user);
    }));
}


Future<Nothing> FetcherProcess::__fetch(
    const hashmap<CommandInfo::URI, Option<shared_ptr<Cache::Entry>>>& entries,
    const ContainerID& containerId,
    const string& sandboxDirectory,
    const string& cacheDirectory,
    const Option<string>& user)
{
  // Now construct the FetcherInfo based on which URIs we're using
  // the cache for and which ones we are bypassing the cache.
  FetcherInfo info;

  foreachpair (const CommandInfo::URI& uri,
               const Option<shared_ptr<Cache::Entry>>& entry,
               entries) {
    FetcherInfo::Item* item = info.add_items();

    item->mutable_uri()->CopyFrom(uri);

    if (entry.isSome()) {
      if (entry.get()->completion().isPending()) {
        // Since the entry is not yet "complete", i.e.,
        // 'completion().isPending()', it must be the case that we created
        // the entry in FetcherProcess::fetch(). Otherwise the entry should
        // have been in the cache already and we would have waited for its
        // completion in FetcherProcess::fetch().
        item->set_action(FetcherInfo::Item::DOWNLOAD_AND_CACHE);
        item->set_cache_filename(entry.get()->filename);
      } else {
        CHECK_READY(entry.get()->completion());
        item->set_action(FetcherInfo::Item::RETRIEVE_FROM_CACHE);
        item->set_cache_filename(entry.get()->filename);
      }
    } else {
      item->set_action(FetcherInfo::Item::BYPASS_CACHE);
    }
  }

  info.set_sandbox_directory(sandboxDirectory);
  info.set_cache_directory(cacheDirectory);

  if (user.isSome()) {
    info.set_user(user.get());
  }

  if (!flags.frameworks_home.empty()) {
    info.set_frameworks_home(flags.frameworks_home);
  }

  info.mutable_stall_timeout()
    ->set_nanoseconds(flags.fetcher_stall_timeout.ns());

  return run(containerId, sandboxDirectory, user, info)
    .repair(defer(self(), [=](const Future<Nothing>& future) {
      ++metrics.task_fetches_failed;

      LOG(ERROR) << "Failed to run mesos-fetcher: " << future.failure();

      foreachvalue (const Option<shared_ptr<Cache::Entry>>& entry, entries) {
        if (entry.isSome()) {
          entry.get()->unreference();

          if (entry.get()->completion().isPending()) {
            // Unsuccessfully (or partially) downloaded! Remove from the cache.
            entry.get()->fail();
            cache.remove(entry.get()); // Might delete partial download.
          }
        }
      }

      return future; // Always propagate the failure!
    })
    // Call to `operator` here forces the conversion on MSVC. This is implicit
    // on clang and gcc.
    .operator std::function<process::Future<Nothing>(
        const process::Future<Nothing> &)>())
    .then(defer(self(), [=]() {
      ++metrics.task_fetches_succeeded;

      foreachvalue (const Option<shared_ptr<Cache::Entry>>& entry, entries) {
        if (entry.isSome()) {
          entry.get()->unreference();

          if (entry.get()->completion().isPending()) {
            // Successfully downloaded and cached!

            Try<Nothing> adjust = cache.adjust(entry.get());
            if (adjust.isSome()) {
              entry.get()->complete();
            } else {
              LOG(WARNING) << "Failed to adjust the cache size for entry '"
                           << entry.get()->key << "' with error: "
                           << adjust.error();

              // Successfully fetched, but not reusable from the
              // cache, because we are deleting the entry now.
              entry.get()->fail();
              cache.remove(entry.get());
            }
          }
        }
      }

      return Nothing();
    }));
}


static off_t delta(
    const Bytes& actualSize,
    const shared_ptr<FetcherProcess::Cache::Entry>& entry)
{
  if (actualSize < entry->size) {
    Bytes delta = entry->size - actualSize;
    LOG(WARNING) << "URI download result for '" << entry->key
                 << "' is smaller than expected by " << stringify(delta)
                 << " at: " << entry->path();

    return -off_t(delta.bytes());
  } else if (actualSize > entry->size) {
    Bytes delta = actualSize - entry->size;
    LOG(WARNING) << "URI download result for '" << entry->key
                 << "' is larger than expected by " << stringify(delta)
                 << " at: " << entry->path();

    return off_t(delta.bytes());
  }

  return 0;
}


// For testing only.
Try<list<Path>> FetcherProcess::cacheFiles() const
{
  list<Path> result;

  if (!os::exists(flags.fetcher_cache_dir)) {
    return result;
  }

  const Try<list<string>> find =
    os::find(flags.fetcher_cache_dir, CACHE_FILE_NAME_PREFIX);

  if (find.isError()) {
    return Error("Could not access cache directory '" +
                 flags.fetcher_cache_dir + "' with error: " + find.error());
  }

  transform(
      find->begin(),
      find->end(),
      std::back_inserter(result),
      [](const string& path) { return Path(path); });

  return result;
}


// For testing only.
size_t FetcherProcess::cacheSize() const
{
  return cache.size();
}


Bytes FetcherProcess::availableCacheSpace() const
{
  return cache.availableSpace();
}


Future<shared_ptr<FetcherProcess::Cache::Entry>>
FetcherProcess::reserveCacheSpace(
    const Try<Bytes>& requestedSpace,
    const shared_ptr<FetcherProcess::Cache::Entry>& entry)
{
  if (requestedSpace.isError()) {
    // Let anyone waiting on this future know that we've
    // failed to download and they should bypass the cache
    // (any new requests will try again).
    entry->fail();
    cache.remove(entry);

    return Failure("Could not determine size of cache file for '" +
                   entry->key + "' with error: " +
                   requestedSpace.error());
  }

  Try<Nothing> reservation = cache.reserve(requestedSpace.get());

  if (reservation.isError()) {
    // Let anyone waiting on this future know that we've
    // failed to download and they should bypass the cache
    // (any new requests will try again).
    entry->fail();
    cache.remove(entry);

    return Failure("Failed to reserve space in the cache: " +
                   reservation.error());
  }

  VLOG(1) << "Claiming fetcher cache space for: " << entry->key;

  cache.claimSpace(requestedSpace.get());

  // NOTE: We must set the entry size only when we are also claiming
  // the space! Other functions rely on this dependency (see
  // Cache::remove()).
  entry->size = requestedSpace.get();

  return entry;
}


Future<Nothing> FetcherProcess::run(
    const ContainerID& containerId,
    const string& sandboxDirectory,
    const Option<string>& user,
    const FetcherInfo& info)
{
  // Before we fetch let's make sure we create 'stdout' and 'stderr'
  // files into which we can redirect the output of the mesos-fetcher
  // (and later redirect the child's stdout/stderr).

  // TODO(tillt): Considering updating fetcher::run to take paths
  // instead of file descriptors and then use Subprocess::PATH()
  // instead of Subprocess::FD(). The reason this can't easily be done
  // today is because we not only need to open the files but also
  // chown them.
  const string stdoutPath = path::join(info.sandbox_directory(), "stdout");

  Try<int_fd> out = os::open(
      stdoutPath,
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (out.isError()) {
    return Failure("Failed to create 'stdout' file: " + out.error());
  }

  string stderrPath = path::join(info.sandbox_directory(), "stderr");
  Try<int_fd> err = os::open(
      stderrPath,
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (err.isError()) {
    os::close(out.get());
    return Failure("Failed to create 'stderr' file: " + err.error());
  }

// NOTE: `os::chown` is not supported on Windows. The flag that gets passed in
// here is conditionally compiled out on Windows.
#ifndef __WINDOWS__
  if (user.isSome()) {
    // TODO(megha.sharma): Fetcher should not create separate stdout/stderr
    // files but rather use FDs prepared by the container logger.
    // See MESOS-6271 for more details.
    Try<Nothing> chownOut = os::chown(
        user.get(),
        stdoutPath,
        false);

    if (chownOut.isError()) {
      os::close(out.get());
      os::close(err.get());
      return Failure(
          "Failed to chown '" +
          stdoutPath +
          "' to user '" + user.get() + "' : " +
          chownOut.error());
    }

    Try<Nothing> chownErr = os::chown(
        user.get(),
        stderrPath,
        false);

    if (chownErr.isError()) {
      os::close(out.get());
      os::close(err.get());
      return Failure(
          "Failed to chown '" +
          stderrPath +
          "' to user '" + user.get() + "' : " +
          chownErr.error());
    }
  }
#endif // __WINDOWS__

  // Return early if there are no URIs to fetch.
  if (info.items_size() == 0) {
      os::close(out.get());
      os::close(err.get());
      return Nothing();
  }

#ifdef __WINDOWS__
  string fetcherPath = path::join(flags.launcher_dir, "mesos-fetcher.exe");
#else
  string fetcherPath = path::join(flags.launcher_dir, "mesos-fetcher");
#endif // __WINDOWS__

  Result<string> realpath = os::realpath(fetcherPath);

  if (!realpath.isSome()) {
    LOG(ERROR) << "Failed to determine the canonical path "
               << "for the mesos-fetcher '"
               << fetcherPath
               << "': "
               << (realpath.isError() ? realpath.error()
                                      : "No such file or directory");

    os::close(out.get());
    os::close(err.get());

    return Failure("Could not fetch URIs: failed to find mesos-fetcher");
  }

  // Now the actual mesos-fetcher command.
  string command = realpath.get();

  // We pass arguments to the fetcher program by means of an
  // environment variable.
  // For assuring that we pass on variables that may be consumed by
  // the mesos-fetcher, we whitelist them before masking out any
  // unwanted agent->fetcher environment spillover.
  // TODO(tillt): Consider using the `mesos::internal::logging::Flags`
  // to determine the whitelist.
  const hashset<string> whitelist = {
    "MESOS_EXTERNAL_LOG_FILE",
    "MESOS_INITIALIZE_DRIVER_LOGGING",
    "MESOS_LOG_DIR",
    "MESOS_LOGBUFSECS",
    "MESOS_LOGGING_LEVEL",
    "MESOS_QUIET"
  };

  map<string, string> environment;
  foreachpair (const string& key, const string& value, os::environment()) {
    if (whitelist.contains(strings::upper(key)) ||
        (!startsWith(key, "LIBPROCESS_") && !startsWith(key, "MESOS_"))) {
      environment.emplace(key, value);
    }
  }

  environment["MESOS_FETCHER_INFO"] = stringify(JSON::protobuf(info));

  if (flags.hadoop_home.isSome()) {
    environment["HADOOP_HOME"] = flags.hadoop_home.get();
  }

  // TODO(jieyu): This is to make sure the libprocess of the fetcher
  // can properly initialize and find the IP. Since we don't need to
  // use the TCP socket for communication, it's OK to use a local
  // address. Consider disable TCP socket in libprocess if libprocess
  // supports that.
  environment.emplace("LIBPROCESS_IP", "127.0.0.1");

  VLOG(1) << "Fetching URIs using command '" << command << "'";

  Try<Subprocess> fetcherSubprocess = subprocess(
      command,
      Subprocess::PIPE(),
      Subprocess::FD(out.get(), Subprocess::IO::OWNED),
      Subprocess::FD(err.get(), Subprocess::IO::OWNED),
      environment);

  if (fetcherSubprocess.isError()) {
    return Failure("Failed to execute mesos-fetcher: " +
                   fetcherSubprocess.error());
  }

  // Remember this PID in case we need to kill the subprocess. See
  // FetcherProcess::kill(). This value gets removed after we wait on
  // the subprocess.
  subprocessPids[containerId] = fetcherSubprocess->pid();

  return fetcherSubprocess->status()
    .then(defer(self(), [=](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("No status available from mesos-fetcher");
      }

      if (!WSUCCEEDED(status.get())) {
        return Failure("Failed to fetch all URIs for container '" +
                       stringify(containerId) + "': " +
                       WSTRINGIFY(status.get()));
      }

      return Nothing();
    }))
    .onFailed(defer(self(), [=](const string&) {
      // To aid debugging what went wrong when attempting to fetch, grab the
      // fetcher's local log output from the sandbox and log it here.
      Try<string> text = os::read(stderrPath);
      if (text.isSome()) {
        LOG(WARNING) << "Begin fetcher log (stderr in sandbox) for container "
                     << containerId << " from running command: " << command
                     << "\n" << text.get() << "\n"
                     << "End fetcher log for container " << containerId;
      } else {
        LOG(ERROR) << "Fetcher log (stderr in sandbox) for container "
                   << containerId << " not readable: " << text.error();
      }
    }))
    .onAny(defer(self(), [=](const Future<Nothing>&) {
      // Clear the subprocess PID remembered from running mesos-fetcher.
      subprocessPids.erase(containerId);
    }));
}


void FetcherProcess::kill(const ContainerID& containerId)
{
  if (subprocessPids.contains(containerId)) {
    VLOG(1) << "Killing the fetcher for container '" << containerId << "'";
    // Best effort kill the entire fetcher tree.
    os::killtree(subprocessPids.at(containerId), SIGKILL);

    subprocessPids.erase(containerId);
  }
}


string FetcherProcess::Cache::nextFilename(const CommandInfo::URI& uri)
{
  // Different URIs may have the same base name, so we need to
  // segregate the download results. This can be done by separate
  // directories or by different file names. We opt for the latter
  // since there may be tighter limits on how many sub-directories a
  // file system can bear than on how many files can be in a directory.

  // We put a fixed prefix upfront before the serial number so we can
  // later easily find cache files with os::find() to support testing.

  // Why we keep the file extension here: When fetching from cache, if
  // extraction is enabled, the extraction algorithm can look at the
  // extension of the cache file the same way as it would at a
  // download of the original URI, and external commands performing
  // the extraction do not get confused by their source file
  // missing an expected form of extension. This is included in the
  // following.

  // Just for human operators who want to take a look at the cache
  // and relate cache files to URIs, we also add some of the URI's
  // basename, but not too much so we do not exceed file name size
  // limits.

  Try<string> base = Fetcher::basename(uri.value());
  CHECK_SOME(base);

  string s = base.get();
  if (s.size() > 20) {
    // Grab only a prefix and a suffix, but for sure including the
    // file extension.
    s = s.substr(0, 10) + "_" + s.substr(s.size() - 10, string::npos);
  }

  ++filenameSerial;

  return CACHE_FILE_NAME_PREFIX + stringify(filenameSerial) + "-" + s;
}


static string cacheKey(const Option<string>& user, const string& uri)
{
  return user.isNone() ? uri : user.get() + "@" + uri;
}


shared_ptr<FetcherProcess::Cache::Entry> FetcherProcess::Cache::create(
    const string& cacheDirectory,
    const Option<string>& user,
    const CommandInfo::URI& uri)
{
  const string key = cacheKey(user, uri.value());
  const string filename = nextFilename(uri);

  auto entry = shared_ptr<Cache::Entry>(
      new Cache::Entry(key, cacheDirectory, filename));

  table.put(key, entry);
  lruSortedEntries.push_back(entry);

  VLOG(1) << "Created cache entry '" << key << "' with file: " << filename;

  return entry;
}


Option<shared_ptr<FetcherProcess::Cache::Entry>>
FetcherProcess::Cache::get(
    const Option<string>& user,
    const string& uri)
{
  const string key = cacheKey(user, uri);

  Option<shared_ptr<Entry>> entry = table.get(key);
  if (entry.isSome()) {
    // The FetcherProcess will always remove a failed download
    // synchronously after marking this future as failed.
    CHECK(!entry.get()->completion().isFailed());

    // Validate the cache file, if it has been downloaded.
    if (entry.get()->completion().isReady()) {
      Try<Nothing> validation = validate(entry.get());
      if (validation.isError()) {
        LOG(WARNING) << "Validation failed: '" + validation.error() +
                        "'. Removing cache entry...";

        remove(entry.get());
        return None();
      }
    }

    // Refresh the cache entry by moving it to the back of lruSortedEntries.
    lruSortedEntries.remove(entry.get());
    lruSortedEntries.push_back(entry.get());
  }

  return entry;
}


bool FetcherProcess::Cache::contains(
    const Option<string>& user,
    const string& uri) const
{
  const string key = cacheKey(user, uri);
  return table.contains(key);
}


bool FetcherProcess::Cache::contains(
    const shared_ptr<Cache::Entry>& entry) const
{
  Option<shared_ptr<Cache::Entry>> found = table.get(entry->key);
  if (found.isNone()) {
    return false;
  }

  return found == entry;
}


// We are removing an entry if:
//
//   (1) We failed to determine its prospective cache file size.
//   (2) We failed to download it when invoking the mesos-fetcher.
//   (3) We're evicting it to make room for another entry.
//   (4) We failed to validate the cache file.
//
// In (1) and (2) the contract is that we'll have failed the entry's
// future before we call remove, so the entry's future should no
// longer be pending.
//
// In (3) it should be the case that the future is no longer pending,
// because we shouldn't be able to evict something if we're
// currently downloading it, because it should have a non-zero
// reference count and therefore the future must either be ready or
// failed in which case this is just case (1) above.
//
// In (4) we explicitly only validate a cache file if the future is
// ready (i.e., the file has been downloaded).
//
// NOTE: It is not necessarily the case that this cache entry has
// zero references because there might be some waiters on the
// downloading of this entry which haven't been able to run and find
// out that the downloading failed.
//
// We want to attempt to delete the file regardless of if it being
// downloaded since it might have been downloaded partially! Deleting
// this file should not be racing with any other downloading or
// deleting because all calls into the cache are serialized by the
// FetcherProcess and since this entry is already in the cache there
// should not be any other conflicting entries or files representing
// this entry. Furthermore every cache file has a unique name. Thus
// no new download conflicts with the manipulation of any pre-existing
// cache content.
Try<Nothing> FetcherProcess::Cache::remove(
    const shared_ptr<Cache::Entry>& entry)
{
  VLOG(1) << "Removing cache entry '" << entry->key
          << "' with filename: " << entry->filename;

  CHECK(!entry->completion().isPending());

  CHECK(contains(entry));

  table.erase(entry->key);
  lruSortedEntries.remove(entry);

  // We may or may not have started downloading. The download may or may
  // not have been partial. In any case, clean up whatever is there.
  if (os::exists(entry->path().string())) {
    Try<Nothing> rm = os::rm(entry->path().string());
    if (rm.isError()) {
      return Error("Could not delete fetcher cache file '" +
                   entry->path().string() + "' with error: " + rm.error() +
                   " for entry '" + entry->key +
                   "', leaking cache space: " + stringify(entry->size));
    }
  }

  // NOTE: There is an assumption that if and only if 'entry->size > 0'
  // then we've claimed cache space for this entry! This currently only
  // gets set in reserveCacheSpace().
  if (entry->size > 0) {
    releaseSpace(entry->size);

    entry->size = 0;
  }

  return Nothing();
}


// Select LRU cache entries for cache eviction.
Try<list<shared_ptr<FetcherProcess::Cache::Entry>>>
FetcherProcess::Cache::selectVictims(const Bytes& requiredSpace)
{
  list<shared_ptr<FetcherProcess::Cache::Entry>> victims;

  Bytes space = 0;

  foreach (const shared_ptr<Cache::Entry>& entry, lruSortedEntries) {
    if (!entry->isReferenced()) {
      victims.push_back(entry);

      space += entry->size;
      if (space >= requiredSpace) {
        return victims;
      }
    }
  }

  return Error("Could not find enough cache files to evict");
}


Try<Nothing> FetcherProcess::Cache::reserve(
    const Bytes& requestedSpace)
{
  if (availableSpace() < requestedSpace) {
    Bytes missingSpace = requestedSpace - availableSpace();

    VLOG(1) << "Freeing up fetcher cache space for: " << missingSpace;

    const Try<list<shared_ptr<Cache::Entry>>> victims =
      selectVictims(missingSpace);

    if (victims.isError()) {
      return Error("Could not free up enough fetcher cache space");
    }

    foreach (const shared_ptr<Cache::Entry>& entry, victims.get()) {
      Try<Nothing> removal = remove(entry);
      if (removal.isError()) {
        return Error(removal.error());
      }
    }
  }

  return Nothing();
}


Try<Nothing> FetcherProcess::Cache::validate(
    const std::shared_ptr<Cache::Entry>& entry)
{
    VLOG(1) << "Validating cache entry '" << entry->key
            << "' with filename: " << entry->filename;

    if (!os::exists(entry->path().string())) {
      return Error("Cache file does not exist: " + entry->filename);
    }

    // TODO(abudnik): Consider adding validation of the cache file by either:
    //   1. Comparing a known file checksum with the actual checksum of the file
    //      stored on disk.
    //   2. Reading the whole file by chunks. Many filesystems detect data
    //      corruptions when reading file's data. As a positive side effect,
    //      the file's data will be loaded into the page cache.
    return Nothing();
}


Try<Nothing> FetcherProcess::Cache::adjust(
    const shared_ptr<FetcherProcess::Cache::Entry>& entry)
{
  CHECK(contains(entry));

  Try<Bytes> size = os::stat::size(
      entry.get()->path().string(),
      os::stat::FollowSymlink::DO_NOT_FOLLOW_SYMLINK);

  if (size.isSome()) {
    off_t d = delta(size.get(), entry);
    if (d <= 0) {
      entry->size = size.get();

      releaseSpace(Bytes(-d));
    } else {
      return Error("More cache size now necessary, not adjusting " +
                   entry->key);
    }
  } else {
    // This should never be caused by Mesos itself, but cannot be excluded.
    return Error("Fetcher cache file for '" + entry->key +
                 "' disappeared from: " + entry->path().string());
  }

  return Nothing();
}


size_t FetcherProcess::Cache::size() const
{
  return table.size();
}


void FetcherProcess::Cache::claimSpace(const Bytes& bytes)
{
  tally += bytes;

  if (tally > space) {
    // Used cache volume space exceeds the maximum amount set by
    // flags.fetcher_cache_size. This may be tolerated temporarily,
    // if there is sufficient physical space available. But it can
    // otherwise cause unspecified system behavior at any moment.
    LOG(WARNING) << "Fetcher cache space overflow - space used: " << tally
                 << ", exceeds total fetcher cache space: " << space;
  }

  VLOG(1) << "Claimed cache space: " << bytes << ", now using: " << tally;
}


void FetcherProcess::Cache::releaseSpace(const Bytes& bytes)
{
  CHECK(bytes <= tally) << "Attempt to release more cache space than in use - "
                        << " requested: " << bytes << ", in use: " << tally;


  tally -= bytes;

  VLOG(1) << "Released cache space: " << bytes << ", now using: " << tally;
}


Bytes FetcherProcess::Cache::totalSpace() const
{
  return space;
}


Bytes FetcherProcess::Cache::usedSpace() const
{
  return tally;
}


Bytes FetcherProcess::Cache::availableSpace() const
{
  if (tally > space) {
    LOG(WARNING) << "Fetcher cache space overflow - space used: " << tally
                 << ", exceeds total fetcher cache space: " << space;
    return 0;
  }

  return space - tally;
}


void FetcherProcess::Cache::Entry::complete()
{
  CHECK_PENDING(promise.future());

  promise.set(Nothing());
}


Future<Nothing> FetcherProcess::Cache::Entry::completion()
{
  return promise.future();
}


void FetcherProcess::Cache::Entry::fail()
{
  CHECK_PENDING(promise.future());

  promise.fail("Could not download to fetcher cache: " + key);
}


void FetcherProcess::Cache::Entry::reference()
{
  referenceCount++;
}


void FetcherProcess::Cache::Entry::unreference()
{
  CHECK(referenceCount > 0);

  referenceCount--;
}


bool FetcherProcess::Cache::Entry::isReferenced() const
{
  return referenceCount > 0;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
