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
  ~MemoryProfiler() override {}

protected:
  void initialize() override;

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
  Future<http::Response> downloadRawProfile(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // Generates and returns a symbolized heap profile.
  Future<http::Response> downloadSymbolizedProfile(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // Generates and returns a call graph in svg format.
  Future<http::Response> downloadGraphProfile(
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
    static Try<DiskArtifact> create(
        const std::string& filename,
        time_t timestamp,
        std::function<Try<Nothing>(const std::string& outputPath)> generator);

    const time_t getId() const;

    std::string getPath() const;

    // Generates an error response if the file doesn't exist, or a download
    // if it does.
    http::Response asHttp() const;

  private:
    DiskArtifact(const std::string& path, time_t id);

    std::string path;
    time_t id;
  };

  // This profile is obtained by telling jemalloc to dump its stats to a file.
  Try<DiskArtifact> rawProfile = Error("Not yet generated");

  // These profiles are obtained by running `jeprof` on the `raw` profile.
  Try<DiskArtifact> symbolizedProfile = Error("Not yet generated");
  Try<DiskArtifact> graphProfile = Error("Not yet generated");
};

} // namespace process {

#endif // __PROCESS_MEMORY_PROFILER_HPP__
