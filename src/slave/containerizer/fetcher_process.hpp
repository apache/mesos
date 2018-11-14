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

#ifndef __SLAVE_CONTAINERIZER_FETCHER_PROCESS_HPP__
#define __SLAVE_CONTAINERIZER_FETCHER_PROCESS_HPP__

#include <list>
#include <memory>
#include <string>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <process/future.hpp>
#include <process/process.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/pull_gauge.hpp>

#include <stout/hashmap.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

class FetcherProcess : public process::Process<FetcherProcess>
{
public:
  explicit FetcherProcess(const Flags& _flags);
  ~FetcherProcess() override;

  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user);

  // Runs the mesos-fetcher, creating a "stdout" and "stderr" file
  // in the given directory, using these for trace output.
  virtual process::Future<Nothing> run(
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const mesos::fetcher::FetcherInfo& info);

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
      bool isReferenced() const;

      // Returns the path in the filesystem where cache entry resides.
      // TODO(bernd-mesos): Remove this construct after refactoring so
      // that the slave flags get injected into the fetcher.
      Path path() const { return Path(path::join(directory, filename)); }

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

    explicit Cache(Bytes _space) : space(_space), tally(0), filenameSerial(0) {}
    virtual ~Cache() {}

    void claimSpace(const Bytes& bytes);
    void releaseSpace(const Bytes& bytes);

    Bytes totalSpace() const;
    Bytes usedSpace() const;
    Bytes availableSpace() const;

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
    bool contains(
        const Option<std::string>& user, const std::string& uri) const;

    // Returns whether this identical entry is in the cache.
    bool contains(const std::shared_ptr<Cache::Entry>& entry) const;

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
    size_t size() const;

  private:
    // Returns whether the cache file exists and not corrupted.
    Try<Nothing> validate(const std::shared_ptr<Cache::Entry>& entry);

    // Maximum storable number of bytes in the cache directory.
    const Bytes space;

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
      const Option<std::string>& user);

  // Returns a list of cache files on disk for the given slave
  // (for all users combined). For testing.
  Try<std::list<Path>> cacheFiles() const;

  // Returns the number of cache entries for the given slave (for all
  // users combined). For testing.
  size_t cacheSize() const;

  // Returns the amount of remaining cache space that is not occupied
  // by cache entries. For testing.
  Bytes availableCacheSpace() const;

private:
  process::Future<Nothing> __fetch(
      const hashmap<CommandInfo::URI,
      Option<std::shared_ptr<Cache::Entry>>>& entries,
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const std::string& cacheDirectory,
      const Option<std::string>& user);

  // Calls Cache::reserve() and returns a ready entry future if successful,
  // else Failure. Claims the space and assigns the entry's size to this
  // amount if and only if successful.
  process::Future<std::shared_ptr<Cache::Entry>> reserveCacheSpace(
      const Try<Bytes>& requestedSpace,
      const std::shared_ptr<Cache::Entry>& entry);

  struct Metrics
  {
    explicit Metrics(FetcherProcess *fetcher);
    ~Metrics();

    // NOTE: These metrics will increment at most once per task. Even if
    // a single task asks for multiple artifacts, the total number of
    // fetches will only go up by one. And if any of those artifacts
    // fail to fetch, the failure count will only increase by one.
    process::metrics::Counter task_fetches_succeeded;
    process::metrics::Counter task_fetches_failed;

    process::metrics::PullGauge cache_size_total_bytes;
    process::metrics::PullGauge cache_size_used_bytes;
  } metrics;

  const Flags flags;

  Cache cache;

  hashmap<ContainerID, pid_t> subprocessPids;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONTAINERIZER_FETCHER_PROCESS_HPP__
