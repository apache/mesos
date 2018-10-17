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

#include <string>
#include <vector>

#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/archiver.hpp>
#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/copyfile.hpp>

#include <mesos/mesos.hpp>

#include <mesos/fetcher/fetcher.hpp>

#include "common/status_utils.hpp"

#include "hdfs/hdfs.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

using namespace process;

using namespace mesos;
using namespace mesos::internal;

using std::string;
using std::vector;

using mesos::fetcher::FetcherInfo;

using mesos::internal::slave::Fetcher;


// Try to extract sourcePath into directory. If sourcePath is
// recognized as an archive it will be extracted and true returned;
// if not recognized then false will be returned. An Error is
// returned if the extraction command fails.
static Try<bool> extract(
    const string& sourcePath,
    const string& destinationDirectory)
{
  Try<Nothing> result = Nothing();

  Option<Subprocess::IO> in = None();
  Option<Subprocess::IO> out = None();
  vector<string> command;

  // Extract any .tar, .tgz, tar.gz, tar.bz2 or zip files.
  if (strings::endsWith(sourcePath, ".tar") ||
      strings::endsWith(sourcePath, ".tgz") ||
      strings::endsWith(sourcePath, ".tar.gz") ||
      strings::endsWith(sourcePath, ".tbz2") ||
      strings::endsWith(sourcePath, ".tar.bz2") ||
      strings::endsWith(sourcePath, ".txz") ||
      strings::endsWith(sourcePath, ".tar.xz") ||
      strings::endsWith(sourcePath, ".zip")) {
    Try<Nothing> result = archiver::extract(sourcePath, destinationDirectory);
    if (result.isError()) {
      return Error(
          "Failed to extract archive '" + sourcePath +
          "' to '" + destinationDirectory + "': " + result.error());
    }
    return true;
  } else if (strings::endsWith(sourcePath, ".gz")) {
    // Unfortunately, libarchive can't extract bare files, so leave this to
    // the 'gunzip' program, if it exists.
    string pathWithoutExtension = sourcePath.substr(0, sourcePath.length() - 3);
    string filename = Path(pathWithoutExtension).basename();
    string destinationPath = path::join(destinationDirectory, filename);

    command = {"gunzip", "-d", "-c"};
    in = Subprocess::PATH(sourcePath);
    out = Subprocess::PATH(destinationPath);
  } else {
    return false;
  }

  CHECK_GT(command.size(), 0u);

  Try<Subprocess> extractProcess = subprocess(
      command[0],
      command,
      in.getOrElse(Subprocess::PATH(os::DEV_NULL)),
      out.getOrElse(Subprocess::FD(STDOUT_FILENO)),
      Subprocess::FD(STDERR_FILENO));

  if (extractProcess.isError()) {
    return Error(
        "Failed to extract '" + sourcePath + "': '" +
        strings::join(" ", command) + "' failed: " +
        extractProcess.error());
  }

  // `status()` never fails or gets discarded.
  int status = extractProcess->status()->get();
  if (!WSUCCEEDED(status)) {
    return Error(
        "Failed to extract '" + sourcePath + "': '" +
        strings::join(" ", command) + "' failed: " +
        WSTRINGIFY(status));
  }

  LOG(INFO) << "Extracted '" << sourcePath << "' into '"
            << destinationDirectory << "'";

  return true;
}


// Attempt to get the uri using the hadoop client.
static Try<string> downloadWithHadoopClient(
    const string& sourceUri,
    const string& destinationPath)
{
  Try<Owned<HDFS>> hdfs = HDFS::create();
  if (hdfs.isError()) {
    return Error("Failed to create HDFS client: " + hdfs.error());
  }

  LOG(INFO) << "Downloading resource with Hadoop client from '" << sourceUri
            << "' to '" << destinationPath << "'";

  Future<Nothing> result = hdfs.get()->copyToLocal(sourceUri, destinationPath);
  result.await();

  if (!result.isReady()) {
    return Error("HDFS copyToLocal failed: " +
                 (result.isFailed() ? result.failure() : "discarded"));
  }

  return destinationPath;
}


static Try<string> downloadWithNet(
    const string& sourceUri,
    const string& destinationPath,
    const Option<Duration>& stallTimeout)
{
  // The net::download function only supports these protocols.
  CHECK(strings::startsWith(sourceUri, "http://")  ||
        strings::startsWith(sourceUri, "https://") ||
        strings::startsWith(sourceUri, "ftp://")   ||
        strings::startsWith(sourceUri, "ftps://"));

  LOG(INFO) << "Downloading resource from '" << sourceUri
            << "' to '" << destinationPath << "'";

  Try<int> code = net::download(sourceUri, destinationPath, stallTimeout);
  if (code.isError()) {
    return Error("Error downloading resource: " + code.error());
  } else {
    // The status code for successful HTTP requests is 200, the status code
    // for successful FTP file transfers is 226.
    if (strings::startsWith(sourceUri, "ftp://") ||
        strings::startsWith(sourceUri, "ftps://")) {
      if (code.get() != 226) {
        return Error("Error downloading resource, received FTP return code " +
                     stringify(code.get()));
      }
    } else {
      if (code.get() != 200) {
        return Error("Error downloading resource, received HTTP return code " +
                     stringify(code.get()));
      }
    }
  }

  return destinationPath;
}


// TODO(coffler): Refactor code to eliminate redundant function.
static Try<string> copyFile(
    const string& sourcePath, const string& destinationPath)
{
  const Try<Nothing> result = os::copyfile(sourcePath, destinationPath);

  if (result.isError()) {
    return Error(result.error());
  }

  return destinationPath;
}


static Try<string> download(
    const string& _sourceUri,
    const string& destinationPath,
    const Option<string>& frameworksHome,
    const Option<Duration>& stallTimeout)
{
  // Trim leading whitespace for 'sourceUri'.
  const string sourceUri = strings::trim(_sourceUri, strings::PREFIX);

  Try<Nothing> validation = Fetcher::validateUri(sourceUri);
  if (validation.isError()) {
    return Error(validation.error());
  }

  // 1. Try to fetch using a local copy.
  // We regard as local: "file://" or the absence of any URI scheme.
  Result<string> sourcePath =
    Fetcher::uriToLocalPath(sourceUri, frameworksHome);

  if (sourcePath.isError()) {
    return Error(sourcePath.error());
  } else if (sourcePath.isSome()) {
    return copyFile(sourcePath.get(), destinationPath);
  }

  // 2. Try to fetch URI using os::net / libcurl implementation.
  // We consider http, https, ftp, ftps compatible with libcurl.
  if (Fetcher::isNetUri(sourceUri)) {
    return downloadWithNet(sourceUri, destinationPath, stallTimeout);
  }

  // 3. Try to fetch the URI using hadoop client.
  // We use the hadoop client to fetch any URIs that are not
  // handled by other fetchers(local / os::net). These URIs may be
  // `hdfs://` URIs or any other URI that has been configured (and
  // hence handled) in the hadoop client. This allows mesos to
  // externalize the handling of previously unknown resource
  // endpoints without the need to support them natively.
  // Note: Hadoop Client is not a hard dependency for running mesos.
  // This allows users to get mesos up and running without a
  // hadoop_home or the hadoop client setup but in case we reach
  // this part and don't have one configured, the fetch would fail
  // and log an appropriate error.
  return downloadWithHadoopClient(sourceUri, destinationPath);
}


// TODO(bernd-mesos): Refactor this into stout so that we can more easily
// chmod an executable. For example, we could define some static flags
// so that someone can do: os::chmod(path, EXECUTABLE_CHMOD_FLAGS).
static Try<string> chmodExecutable(const string& filePath)
{
  // TODO(coffler): Fix Windows chmod handling, see MESOS-3176.
#ifndef __WINDOWS__
  Try<Nothing> chmod = os::chmod(
      filePath, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
  if (chmod.isError()) {
    return Error("Failed to chmod executable '" +
                 filePath + "': " + chmod.error());
  }
#endif // __WINDOWS__

  return filePath;
}


// Returns the resulting file or in case of extraction the destination
// directory (for logging).
static Try<string> fetchBypassingCache(
    const CommandInfo::URI& uri,
    const string& sandboxDirectory,
    const Option<string>& frameworksHome,
    const Option<Duration>& stallTimeout)
{
  LOG(INFO) << "Fetching '" << uri.value()
            << "' directly into the sandbox directory";

  // TODO(mrbrowning): Factor out duplicated processing of "output_file" field
  // here and in fetchFromCache into a separate helper function.
  if (uri.has_output_file()) {
    string dirname = Path(uri.output_file()).dirname();
    if (dirname != ".") {
      Try<Nothing> result =
        os::mkdir(path::join(sandboxDirectory, dirname), true);

      if (result.isError()) {
        return Error(
            "Unable to create subdirectory " + dirname +
            " in sandbox: " + result.error());
      }
    }
  }

  Try<string> outputFile = uri.has_output_file()
    ? uri.output_file()
    : Fetcher::basename(uri.value());

  if (outputFile.isError()) {
    return Error(outputFile.error());
  }

  string path = path::join(sandboxDirectory, outputFile.get());

  Try<string> downloaded =
    download(uri.value(), path, frameworksHome, stallTimeout);
  if (downloaded.isError()) {
    return Error(downloaded.error());
  }

  if (uri.executable()) {
    return chmodExecutable(downloaded.get());
  } else if (uri.extract()) {
    Try<bool> extracted = extract(path, sandboxDirectory);
    if (extracted.isError()) {
      return Error(extracted.error());
    } else if (!extracted.get()) {
      LOG(WARNING) << "Copying instead of extracting resource from URI with "
                   << "'extract' flag, because it does not seem to be an "
                   << "archive: " << uri.value();
    }
  }

  return downloaded;
}


// Returns the resulting file or in case of extraction the destination
// directory (for logging).
static Try<string> fetchFromCache(
    const FetcherInfo::Item& item,
    const string& cacheDirectory,
    const string& sandboxDirectory)
{
  LOG(INFO) << "Fetching URI '" << item.uri().value() << "' from cache";

  if (item.uri().has_output_file()) {
    string dirname = Path(item.uri().output_file()).dirname();
    if (dirname != ".") {
      Try<Nothing> result =
        os::mkdir(path::join(sandboxDirectory, dirname), true);

      if (result.isError()) {
        return Error(
          "Unable to create subdirectory " + dirname +
          " in sandbox: " + result.error());
      }
    }
  }

  Try<string> outputFile = item.uri().has_output_file()
    ? item.uri().output_file()
    : Fetcher::basename(item.uri().value());

  if (outputFile.isError()) {
    return Error(outputFile.error());
  }

  string destinationPath = path::join(sandboxDirectory, outputFile.get());

  // Non-empty cache filename is guaranteed by the callers of this function.
  CHECK(!item.cache_filename().empty());

  string sourcePath = path::join(cacheDirectory, item.cache_filename());

  if (item.uri().executable()) {
    Try<string> copied = copyFile(sourcePath, destinationPath);
    if (copied.isError()) {
      return Error(copied.error());
    }

    return chmodExecutable(copied.get());
  } else if (item.uri().extract()) {
    Try<bool> extracted = extract(sourcePath, sandboxDirectory);
    if (extracted.isError()) {
      return Error(extracted.error());
    } else if (extracted.get()) {
      return sandboxDirectory;
    } else {
      LOG(WARNING) << "Copying instead of extracting resource from URI with "
                   << "'extract' flag, because it does not seem to be an "
                   << "archive: " << item.uri().value();
    }
  }

  return copyFile(sourcePath, destinationPath);
}


// Returns the resulting file or in case of extraction the destination
// directory (for logging).
static Try<string> fetchThroughCache(
    const FetcherInfo::Item& item,
    const Option<string>& cacheDirectory,
    const string& sandboxDirectory,
    const Option<string>& frameworksHome,
    const Option<Duration>& stallTimeout)
{
  if (cacheDirectory.isNone() || cacheDirectory->empty()) {
    return Error("Cache directory not specified");
  }

  if (!item.has_cache_filename() || item.cache_filename().empty()) {
    // This should never happen if this program is used by the Mesos
    // slave and could then be a CHECK. But other uses are possible.
    return Error("No cache file name for: " + item.uri().value());
  }

  CHECK_NE(FetcherInfo::Item::BYPASS_CACHE, item.action())
    << "Unexpected fetcher action selector";

  CHECK(os::exists(cacheDirectory.get()))
    << "Fetcher cache directory was expected to exist but was not found";

  if (item.action() == FetcherInfo::Item::DOWNLOAD_AND_CACHE) {
    const string cachePath =
      path::join(cacheDirectory.get(), item.cache_filename());

    if (!os::exists(cachePath)) {
      Try<string> downloaded = download(
          item.uri().value(),
          cachePath,
          frameworksHome,
          stallTimeout);

      if (downloaded.isError()) {
        return Error(downloaded.error());
      }
    }
  }

  return fetchFromCache(item, cacheDirectory.get(), sandboxDirectory);
}


// Returns the resulting file or in case of extraction the destination
// directory (for logging).
static Try<string> fetch(
    const FetcherInfo::Item& item,
    const Option<string>& cacheDirectory,
    const string& sandboxDirectory,
    const Option<string>& frameworksHome,
    const Option<Duration>& stallTimeout)
{
  LOG(INFO) << "Fetching URI '" << item.uri().value() << "'";

  if (item.action() == FetcherInfo::Item::BYPASS_CACHE) {
    return fetchBypassingCache(
        item.uri(),
        sandboxDirectory,
        frameworksHome,
        stallTimeout);
  }

  return fetchThroughCache(
      item,
      cacheDirectory,
      sandboxDirectory,
      frameworksHome,
      stallTimeout);
}


// Checks to see if it's necessary to create a fetcher cache directory for this
// user, and creates it if so.
static Try<Nothing> createCacheDirectory(const FetcherInfo& fetcherInfo)
{
  if (!fetcherInfo.has_cache_directory()) {
    return Nothing();
  }

  foreach (const FetcherInfo::Item& item, fetcherInfo.items()) {
    if (item.action() != FetcherInfo::Item::BYPASS_CACHE) {
      // If this user has fetched anything into the cache before, their cache
      // directory will already exist. Set `recursive = true` when calling
      // `os::mkdir` to ensure no error is returned in this case.
      Try<Nothing> mkdir = os::mkdir(fetcherInfo.cache_directory(), true);
      if (mkdir.isError()) {
        return mkdir;
      }

      if (fetcherInfo.has_user()) {
        // Fetching is performed as the task's user,
        // so chown the cache directory.

        // TODO(coffler): Fix Windows chown handling, see MESOS-8063.
#ifndef __WINDOWS__
        Try<Nothing> chown = os::chown(
            fetcherInfo.user(),
            fetcherInfo.cache_directory(),
            false);

        if (chown.isError()) {
          return chown;
        }
#endif // __WINDOWS__
      }

      break;
    }
  }

  return Nothing();
}


// This "fetcher program" is invoked by the slave's fetcher actor
// (Fetcher, FetcherProcess) to "fetch" URIs into the sandbox directory
// of a given task. Its parameters are provided in the form of the env
// var MESOS_FETCHER_INFO which contains a FetcherInfo (see
// fetcher.proto) object formatted in JSON. These are set by the actor
// to indicate what set of URIs to process and how to proceed with
// each one. A URI can be downloaded directly to the task's sandbox
// directory or it can be copied to a cache first or it can be reused
// from the cache, avoiding downloading. All cache management and
// bookkeeping is centralized in the slave's fetcher actor, which can
// have multiple instances of this fetcher program running at any
// given time. Exit code: 0 if entirely successful, otherwise 1.
int main(int argc, char* argv[])
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  mesos::internal::logging::Flags flags;

  Try<flags::Warnings> load = flags.load("MESOS_", argc, argv);

  if (flags.help) {
    std::cout << flags.usage() << std::endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    std::cerr << flags.usage(load.error()) << std::endl;
    return EXIT_FAILURE;
  }

  logging::initialize(argv[0], true, flags); // Catch signals.

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  const Option<string> jsonFetcherInfo = os::getenv("MESOS_FETCHER_INFO");
  CHECK_SOME(jsonFetcherInfo)
    << "Missing MESOS_FETCHER_INFO environment variable";

  LOG(INFO) << "Fetcher Info: " << jsonFetcherInfo.get();

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(jsonFetcherInfo.get());
  CHECK_SOME(parse) << "Failed to parse MESOS_FETCHER_INFO: " << parse.error();

  Try<FetcherInfo> fetcherInfo = ::protobuf::parse<FetcherInfo>(parse.get());
  CHECK_SOME(fetcherInfo)
    << "Failed to parse FetcherInfo: " << fetcherInfo.error();

  CHECK(!fetcherInfo->sandbox_directory().empty())
    << "Missing sandbox directory";

  const string sandboxDirectory = fetcherInfo->sandbox_directory();

  Try<Nothing> result = createCacheDirectory(fetcherInfo.get());
  if (result.isError()) {
    EXIT(EXIT_FAILURE)
      << "Could not create the fetcher cache directory: " << result.error();
  }

  // If the `FetcherInfo` specifies a user, use `os::su()` to fetch files as the
  // task's user to ensure that filesystem permissions are enforced.
  if (fetcherInfo->has_user()) {
  // TODO(coffler): No support for os::su on Windows, see MESOS-8063
#ifndef __WINDOWS__
    result = os::su(fetcherInfo->user());
    if (result.isError()) {
      EXIT(EXIT_FAILURE) << "Fetcher could not execute `os::su()` for user '"
                         << fetcherInfo->user() << "'";
    }
#endif // __WINDOWS__
  }

  const Option<string> cacheDirectory =
    fetcherInfo->has_cache_directory()
      ? Option<string>::some(fetcherInfo->cache_directory())
      : Option<string>::none();

  const Option<string> frameworksHome =
    fetcherInfo->has_frameworks_home()
      ? Option<string>::some(fetcherInfo->frameworks_home())
      : Option<string>::none();

  const Option<Duration> stallTimeout =
    fetcherInfo->has_stall_timeout()
      ? Nanoseconds(fetcherInfo->stall_timeout().nanoseconds())
      : Option<Duration>::none();

  // Fetch each URI to a local file and chmod if necessary.
  foreach (const FetcherInfo::Item& item, fetcherInfo->items()) {
    Try<string> fetched = fetch(
        item, cacheDirectory, sandboxDirectory, frameworksHome, stallTimeout);
    if (fetched.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to fetch '" << item.uri().value() << "': " + fetched.error();
    } else {
      LOG(INFO) << "Fetched '" << item.uri().value()
                << "' to '" << fetched.get() << "'";
    }
  }

  LOG(INFO) << "Successfully fetched all URIs into "
            << "'" << sandboxDirectory << "'";

  return 0;
}
