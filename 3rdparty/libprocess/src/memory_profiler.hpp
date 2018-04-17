// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_MEMORY_PROFILER_HPP__
#define __PROCESS_MEMORY_PROFILER_HPP__

#include <functional>
#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

namespace process {

// This class provides support for memory profiling and introspection
// using the capabilities of the jemalloc memory allocator.
//
// For user-facing documentation on how to use the facilities provided
// by this class, see `docs/memory-profiling.md` in the mesos repository.
//
// For more details about the implementation, see the comments
// in `memory_profiler.cpp`.
class MemoryProfiler : public Process<MemoryProfiler>
{
public:
  MemoryProfiler(const Option<std::string>& authenticationRealm);
  virtual ~MemoryProfiler() {}

protected:
  virtual void initialize();

private:
  static const std::string START_HELP();
  static const std::string STOP_HELP();
  static const std::string DOWNLOAD_RAW_HELP();
  static const std::string DOWNLOAD_TEXT_HELP();
  static const std::string DOWNLOAD_GRAPH_HELP();
  static const std::string STATISTICS_HELP();
  static const std::string STATE_HELP();

  // HTTP endpoints.
  // Refer to the `HELP()` messages for detailed documentation.

  // Starts memory profiling.
  Future<http::Response> start(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // Stops memory profiling and dumps collected data.
  Future<http::Response> stop(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // Returns a raw heap profile.
  Future<http::Response> downloadRaw(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // Generates and returns a symbolized heap profile.
  Future<http::Response> downloadTextProfile(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // Generates and returns a call graph in svg format.
  Future<http::Response> downloadGraph(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // Shows memory allocation statistics.
  Future<http::Response> statistics(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // Shows the configuration of the memory-profiler process.
  Future<http::Response> state(
    const http::Request& request,
    const Option<http::authentication::Principal>&);

  // Internal functions, helper classes, and data members.

  // Deactivates data collection and attempts to dump the raw profile to disk.
  void stopAndGenerateRawProfile();

  // The authentication realm that the profiler's HTTP endpoints will be
  // installed into.
  Option<std::string> authenticationRealm;

  // Stores information about the current profiling run.
  // When the timer reaches zero, a dump of the collected
  // data will be attempted.
  class ProfilingRun
  {
  public:
    ProfilingRun(MemoryProfiler*, time_t, const Duration&);
    void extend(MemoryProfiler*, const Duration&);

    time_t id;
    Timer timer;
  };

  Option<ProfilingRun> currentRun;

  // Represents a file on the filesystem that is generated as the result
  // of running some action.
  class DiskArtifact
  {
  public:
    DiskArtifact(const std::string& filename);

    const Try<time_t>& id() const;

    // This is generated lazily to avoid creating a temporary
    // directory unless we actually need it.
    Try<std::string> path() const;

    // If the file with the given timestamp does not exist on disk, calls the
    // supplied `generator` function that should write it to the specified
    // location.
    Try<Nothing> generate(
        time_t timestamp,
        std::function<Try<Nothing>(const std::string& outputPath)> generator);

    // Generates an error response if the file doesn't exist, or a download
    // if it does.
    http::Response asHttp() const;

  private:
    std::string filename;

    // A timestamp of the last successful creation that serves as unique id,
    // or the reason why it couldn't be created.
    Try<time_t> timestamp;
  };

  DiskArtifact jemallocRawProfile;
  DiskArtifact jeprofSymbolizedProfile;
  DiskArtifact jeprofGraph;
};

} // namespace process {

#endif // __PROCESS_MEMORY_PROFILER_HPP__
