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

#ifndef __SLAVE_CONTAINERIZER_FETCHER_HPP__
#define __SLAVE_CONTAINERIZER_FETCHER_HPP__

#include <list>
#include <string>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/fetcher/fetcher.hpp>

#include <process/id.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/hashmap.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class FetcherProcess;

// Argument passing to and invocation of the external fetcher program.
// Bookkeeping of executor files that are cached after downloading from
// a URI by the fetcher program. Cache eviction. There has to be exactly
// one fetcher with a distinct cache directory per active slave. This
// means that the cache directory can only be fixed after the slave ID
// has been determined by registration or recovery. Downloads to cache
// files are separated on a per-user basis. The cache must only be used
// for URIs for which the expected download size can be determined and
// trusted before downloading. If there is any problem using the cache
// for any given URI, the fetch procedure automatically reverts to
// fetching directly into the sandbox directory.
class Fetcher
{
public:
  // Extracts the basename from a URI. For example, "d.txt" from
  // "htpp://1.2.3.4:5050/a/b/c/d.txt". The current implementation
  // only works for fairly regular-shaped URIs with a "/" and a proper
  // file name at the end.
  static Try<std::string> basename(const std::string& uri);

  // Some checks to make sure using the URI value in shell commands
  // is safe.
  // TODO(benh): These should be pushed into the scheduler driver and
  // reported to the user.
  static Try<Nothing> validateUri(const std::string& uri);

  // Checks to make sure the URI 'output_file' is valid.
  static Try<Nothing> validateOutputFile(const std::string& path);

  // Determines if the given URI refers to a local file system path
  // and prepends frameworksHome if it is a relative path. Fails if
  // frameworksHome is empty and a local path is indicated.
  static Result<std::string> uriToLocalPath(
      const std::string& uri,
      const Option<std::string>& frameworksHome);

  static bool isNetUri(const std::string& uri);

  Fetcher();

  // This is only public for tests.
  Fetcher(const process::Owned<FetcherProcess>& process);

  virtual ~Fetcher();

  // TODO(bernd-mesos): Inject these parameters at Fetcher creation time.
  // Then also inject the fetcher into the slave at creation time. Then
  // it will be possible to make this an instance method instead of a
  // static one for the slave to call during startup or recovery.
  static Try<Nothing> recover(const SlaveID& slaveId, const Flags& flags);

  // Download the URIs specified in the command info and place the
  // resulting files into the given sandbox directory. Chmod said files
  // to the user if given. Send stdout and stderr output to files
  // "stdout" and "stderr" in the given directory. Extract archives and/or
  // use the cache if so instructed by the given CommandInfo::URI items.
  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const Flags& flags);

  // Best effort to kill the fetcher subprocess associated with the
  // indicated container. Do nothing if no such subprocess exists.
  void kill(const ContainerID& containerId);

private:
  process::Owned<FetcherProcess> process;
};


class FetcherProcess : public process::Process<FetcherProcess>
{
public:
  FetcherProcess() : ProcessBase(process::ID::generate("fetcher")) {}

  virtual ~FetcherProcess();

  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const Flags& flags);

  // Runs the mesos-fetcher, creating a "stdout" and "stderr" file
  // in the given directory, using these for trace output.
  virtual process::Future<Nothing> run(
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const mesos::fetcher::FetcherInfo& info,
      const Flags& flags);

  // Best effort attempt to kill the external mesos-fetcher process
  // running on behalf of the given container ID, if any.
  void kill(const ContainerID& containerId);

  // Representation of the fetcher cache and its contents. There is
  // exactly one instance per instance of FetcherProcess. All methods
  // of Cache are to be executed on the latter to ensure atomicity of
  // cache operations.
  class Cache
  {
  public:
    class Entry
    {
    public:
      Entry(
          const std::string& key,
          const std::string& directory,
          const std::string& filename)
        : key(key),
          directory(directory),
          filename(filename),
          size(0),
          referenceCount(0) {}

      ~Entry() {}

      // Marks this file's download as successful by setting its promise
      // to the path of the file in the cache.
      void complete();

      // Indicates whether this file's download into the cache is
      // successfully completed.
      process::Future<Nothing> completion();

      // Marks this download as failed, notifying concurrent fetch attempts
      // waiting for this result, by setting the promise to failed.
      void fail();

      // While an entry is "referenced" it cannot be evicted from the
      // cache.
      void reference();
      void unreference();
      bool isReferenced();

      // Returns the path in the filesystem where cache entry resides.
      // TODO(bernd-mesos): Remove this construct after refactoring so
      // that the slave flags get injected into the fetcher.
      Path path() { return Path(path::join(directory, filename)); }

      // Uniquely identifies a user/URI combination.
      const std::string key;

      // Cache directory where this entry is stored.
      // TODO(bernd-mesos): Remove this construct after refactoring so
      // that the slave flags get injected into the fetcher.
      const std::string directory;

      // The unique name of the file held in the cache on behalf of a
      // URI.
      const std::string filename;

      // The expected size of the cache file. This field is set before
      // downloading. If the actual size of the downloaded file is
      // different a warning is logged and the field's value adjusted.
      Bytes size;

    private:
      // Concurrent fetch attempts can reference the same entry multiple
      // times.
      unsigned long referenceCount;

     // Indicates successful downloading to the cache.
      process::Promise<Nothing> promise;
    };

    Cache() : space(0), tally(0), filenameSerial(0) {}
    virtual ~Cache() {}

    // Registers the maximum usable space in the cache directory.
    // TODO(bernd-mesos): This method will disappear when injecting 'flags'
    // into the fetcher instead of passing 'flags' around as parameter.
    void setSpace(const Bytes& bytes);

    void claimSpace(const Bytes& bytes);
    void releaseSpace(const Bytes& bytes);
    Bytes availableSpace();

    // Invents a new, distinct base name for a cache file, using the same
    // filename extension as the URI.
    std::string nextFilename(const CommandInfo::URI& uri);

    // Creates a new entry and inserts it into the cache table. Also
    // sets its reference count to 1. Returns the entry.
    std::shared_ptr<Entry> create(
        const std::string& cacheDirectory,
        const Option<std::string>& user,
        const CommandInfo::URI& uri);

    // Retrieves the cache entry indexed by the parameters, without
    // changing its reference count.
    Option<std::shared_ptr<Entry>> get(
        const Option<std::string>& user,
        const std::string& uri);

    // Returns whether an entry for this user and URI is in the cache.
    bool contains(const Option<std::string>& user, const std::string& uri);

    // Returns whether this identical entry is in the cache.
    bool contains(const std::shared_ptr<Cache::Entry>& entry);

    // Completely deletes a cache entry and its file. Warns on failure.
    // Virtual for mock testing.
    virtual Try<Nothing> remove(const std::shared_ptr<Entry>& entry);

    // Determines a list of cache entries to remove, respectively cache files
    // to delete, so that at least the required amount of space would become
    // available.
    Try<std::list<std::shared_ptr<Cache::Entry>>>
        selectVictims(const Bytes& requiredSpace);

    // Ensures that there is the requested amount of space is available
    // Evicts other files as necessary to make it so.
    Try<Nothing> reserve(const Bytes& requestedSpace);

    // Finds out if any predictions about cache file sizes have been
    // inaccurate, logs this if so, and records the cache files' actual
    // sizes and adjusts the cache's total amount of space in use.
    Try<Nothing> adjust(const std::shared_ptr<Cache::Entry>& entry);

    // Number of entries.
    size_t size();

  private:
    // Maximum storable number of bytes in the cache directory.
    Bytes space;

    // How much space has been reserved to be occupied by cache files.
    Bytes tally;

    // Used to generate distinct cache file names simply by counting.
    unsigned long filenameSerial;

    // Maps keys (cache directory / URI combinations) to cache file
    // entries.
    hashmap<std::string, std::shared_ptr<Entry>> table;

    // Stores cache file entries sorted from LRU to MRU.
    std::list<std::shared_ptr<Entry>> lruSortedEntries;
  };

  // Public and virtual for mock testing.
  virtual process::Future<Nothing> _fetch(
      const hashmap<
          CommandInfo::URI,
          Option<process::Future<std::shared_ptr<Cache::Entry>>>>&
        entries,
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const std::string& cacheDirectory,
      const Option<std::string>& user,
      const Flags& flags);

  // Returns a list of cache files on disk for the given slave
  // (for all users combined). For testing.
  // TODO(bernd-mesos): Remove the parameters after slave/containerizer
  // refactoring for injection of these.
  Try<std::list<Path>> cacheFiles(const SlaveID& slaveId, const Flags& flags);

  // Returns the number of cache entries for the given slave (for all
  // users combined). For testing.
  size_t cacheSize();

  // Returns the amount of remaining cache space that is not occupied
  // by cache entries. For testing.
  Bytes availableCacheSpace();

private:
  process::Future<Nothing> __fetch(
      const hashmap<CommandInfo::URI,
      Option<std::shared_ptr<Cache::Entry>>>& entries,
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const std::string& cacheDirectory,
      const Option<std::string>& user,
      const Flags& flags);

  // Calls Cache::reserve() and returns a ready entry future if successful,
  // else Failure. Claims the space and assigns the entry's size to this
  // amount if and only if successful.
  process::Future<std::shared_ptr<Cache::Entry>> reserveCacheSpace(
      const Try<Bytes>& requestedSpace,
      const std::shared_ptr<Cache::Entry>& entry);

  Cache cache;

  hashmap<ContainerID, pid_t> subprocessPids;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONTAINERIZER_FETCHER_HPP__
