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

#include "memory_profiler.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>

#include <stout/assert.hpp>
#include <stout/format.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/write.hpp>

#include <glog/logging.h>

using process::Future;
using process::HELP;
using process::TLDR;
using process::DESCRIPTION;
using process::AUTHENTICATION;

using std::string;

// The main workflow to generate and download a heap profile
// goes through the sequence of endpoints
//
//     `/start?duration=T` -> `/download/{raw,graph,text}`
//
// A started profiling run will be stopped automatically after the
// given duration has passed, but can be ended prematurely by accessing
//
//     `/stop`
//
// Any started run has an associated unique id, which is intended to make
// it easier for scripts to reliably download only those profiles that
// they themselves generated. Human operators will mostly ignore it and
// use the provided default value.
//
// The generated files are typically stored under the directory
// `/tmp/libprocess.XXXXXX/jemalloc.{txt,svg,dump}`, where XXXXXX
// stands for a random combination of letters. This directory, as well
// as the files contained therein, is created lazily the first time it
// is accessed.
//
// To avoid running out of disk space, every time a new file is
// generated, the previous one is overwritten. The members `rawId`,
// `graphId` and `textId` track which version, if any, of the
// corresponding artifact is currently available on disk.
//
// Since this class, being a part of libprocess, will end up in
// `libmesos.so` and thus possibly in applications that use their own
// memory allocator, we carefully avoid actually linking this class
// against `libjemalloc.so`. Instead, we use weak symbols to detect the
// presence of jemalloc at runtime, and use a macro to hide these symbols
// when building on platforms that don't support weak symbols.


#ifdef LIBPROCESS_ALLOW_JEMALLOC

extern "C" __attribute__((weak)) void malloc_stats_print(
  void (*writecb)(void*, const char*),
  void* opaque,
  const char* opts);

extern "C" __attribute__((weak)) int mallctl(
  const char* opt, void* oldp, size_t* oldsz, void* newp, size_t newsz);

#endif // LIBPROCESS_ALLOW_JEMALLOC


namespace {

constexpr char LIBPROCESS_DEFAULT_TMPDIR[] = "/tmp";
constexpr char RAW_PROFILE_FILENAME[] = "profile.dump";
constexpr char SYMBOLIZED_PROFILE_FILENAME[] = "symbolized-profile.dump";
constexpr char GRAPH_FILENAME[] = "profile.svg";
constexpr Duration MINIMUM_COLLECTION_TIME = Seconds(1);
constexpr Duration DEFAULT_COLLECTION_TIME = Minutes(5);
constexpr Duration MAXIMUM_COLLECTION_TIME = Hours(24);


constexpr char JEMALLOC_NOT_DETECTED_MESSAGE[] = R"_(
The current binary doesn't seem to be linked against jemalloc,
or the currently used jemalloc library was compiled without
support for statistics collection.

If the current binary was not compiled against jemalloc,
consider adding the path to libjemalloc to the LD_PRELOAD
environment variable, for example LD_PRELOAD=/usr/lib/libjemalloc.so

If you're running a mesos binary and want to have it linked
against jemalloc by default, consider using the
--enable-jemalloc-allocator configuration option)_";


constexpr char JEMALLOC_PROFILING_NOT_ENABLED_MESSAGE[] = R"_(
The current process seems to be using jemalloc, but profiling
couldn't be enabled.

If you're using a custom version of libjemalloc, make sure that
MALLOC_CONF="prof:true" is part of the environment. (The '/state'
endpoint can be used to double-check the current malloc
configuration).

If the environment looks correct, make sure jemalloc was built
with the --enable-stats and --enable-prof options enabled.

If you're running a mesos binary that was built with the
--enable-memory-profiling option enabled and you're still seeing
this message, please consider filing a bug report)_";


// Size in bytes of the dummy file that gets written when hitting `/start`.
constexpr int DUMMY_FILE_SIZE = 64 * 1024; // 64 KiB


// The `detectJemalloc()` function below was taken from the folly library
// (called `usingJEMalloc()` there), originally distributed by Facebook, Inc
// under the Apache License.
//
// It checks whether jemalloc is used as the current malloc implementation by
// allocating one byte and checking if the threads allocation counter increases.
// This requires jemalloc to have been compiled with `--enable-stats`.
bool detectJemalloc() noexcept {
#ifndef LIBPROCESS_ALLOW_JEMALLOC
  return false;
#else
  static const bool result = [] () noexcept {
    // Some platforms (*cough* OSX *cough*) require weak symbol checks to be
    // in the form if (mallctl != nullptr). Not if (mallctl) or if (!mallctl)
    // (!!). http://goo.gl/xpmctm
    if (mallctl == nullptr || malloc_stats_print == nullptr) {
      return false;
    }

    // "volatile" because gcc optimizes out the reads from *counter, because
    // it "knows" malloc doesn't modify global state...
    volatile uint64_t* counter;
    size_t counterLen = sizeof(uint64_t*);

    if (mallctl("thread.allocatedp", static_cast<void*>(&counter), &counterLen,
                nullptr, 0) != 0) {
      return false;
    }

    if (counterLen != sizeof(uint64_t*)) {
      return false;
    }

    uint64_t origAllocated = *counter;

    // Static because otherwise clever compilers will find out that
    // the ptr is not used and does not escape the scope, so they will
    // just optimize away the malloc.
    static const void* ptr = malloc(1);
    if (!ptr) {
      // wtf, failing to allocate 1 byte
      return false;
    }

    return (origAllocated != *counter);
  }();

  return result;
#endif
}


template<typename T>
Try<T> readJemallocSetting(const char* name)
{
#ifdef LIBPROCESS_ALLOW_JEMALLOC
  if (!detectJemalloc()) {
    return Error(JEMALLOC_NOT_DETECTED_MESSAGE);
  }

  T value;
  size_t size = sizeof(value);
  int error = ::mallctl(name, &value, &size, nullptr, 0);

  if (error) {
    return Error(strings::format(
       "Couldn't read option %s: %s", name, ::strerror(error)).get());
  }

  return value;
#else
  UNREACHABLE();
#endif
}


// Returns an error on failure or the previous value on success.
template<typename T>
Try<T> updateJemallocSetting(const char* name, const T& value)
{
#ifdef LIBPROCESS_ALLOW_JEMALLOC
  if (!detectJemalloc()) {
    return Error(JEMALLOC_NOT_DETECTED_MESSAGE);
  }

  T previous;
  size_t size = sizeof(previous);
  int error = ::mallctl(
      name, &previous, &size, const_cast<T*>(&value), sizeof(value));

  if (error) {
    return Error(strings::format(
        "Couldn't write value %s for option %s: %s",
        stringify(value), name, ::strerror(error)).get());
  }

  return previous;
#else
  UNREACHABLE();
#endif
}


// Sadly, we cannot just use `updateJemallocSetting()` and ignore the result,
// because some settings, in particular `prof.dump`, don't have previous value
// to return.
template<typename T>
Try<Nothing> writeJemallocSetting(const char* name, const T& value)
{
#ifdef LIBPROCESS_ALLOW_JEMALLOC
  if (!detectJemalloc()) {
    return Error(JEMALLOC_NOT_DETECTED_MESSAGE);
  }

  int error = mallctl(
      name, nullptr, nullptr, const_cast<T*>(&value), sizeof(value));

  if (error) {
    return Error(strings::format(
        "Couldn't write value %s for option %s: %s",
        stringify(value), name, ::strerror(error)).get());
  }

  return Nothing();
#else
  UNREACHABLE();
#endif
}


// All generated disk artifacts, i.e. profiles and graph files, are stored
// in this directory. It is generated lazily on the first call to
// `getTemporaryDirectoryPath()` and is never changed afterwards.
// Locking is not needed because all accesses go through the same instance of
// the `MemoryProfiler` process and hence are always serialized with respect
// to each other.
//
// TODO(bevers): This should be made available libprocess-global eventually,
// but right now this is the only class that has a use for it.
Option<Path> temporaryDirectory;


Try<Path> getTemporaryDirectoryPath() {
  if (temporaryDirectory.isSome()) {
    return temporaryDirectory.get();
  }

  // TODO(bevers): Add a libprocess-specific override for the system-wide
  // `TMPDIR`, for example `LIBPROCESS_TMPDIR`.
  const string tmpdir =
    os::getenv("TMPDIR").getOrElse(LIBPROCESS_DEFAULT_TMPDIR);

  const string pathTemplate = path::join(tmpdir, "libprocess.XXXXXX");

  // TODO(bevers): Add an atexit-handler that cleans up the directory.
  Try<string> dir = os::mkdtemp(pathTemplate);
  if (dir.isError()) {
    return Error(dir.error());
  }

  temporaryDirectory = dir.get();

  VLOG(1) << "Using path " << dir.get() << " to store temporary files";

  return temporaryDirectory.get();
}


// TODO(alexr): Consider making this asynchronous.
Try<Nothing> generateJeprofFile(
    const string& inputPath,
    const string& options,
    const string& outputPath)
{
  // As jeprof doesn't have an option to specify an output file, we actually
  // cannot use `os::spawn()` here.
  // Note that the three parameters *MUST NOT* be controllable by the user
  // accessing the HTTP endpoints, otherwise arbitrary shell commands could be
  // trivially injected.
  // Apart from that, we dont need to be as careful here as with the actual
  // heap profile dump, because a failure will not crash the whole process.
  Option<int> result = os::system(strings::format(
      "jeprof %s /proc/self/exe %s > %s",
      options,
      inputPath,
      outputPath).get());

  if (result != 0) {
    return Error(
      "Error trying to run jeprof. Please make sure that jeprof is installed"
      " and that the input file contains data. For more information, please"
      " consult the log files of this process");
  }

  return Nothing();
}


// TODO(bevers): Implement `http::Request::extractFromRequest<T>(string key)`
// instead of having this here.
Result<time_t> extractIdFromRequest(const process::http::Request& request)
{
  Option<string> idParameter = request.url.query.get("id");
  if (idParameter.isNone()) {
    return None();
  }

  // Since `strtoll()` can legitimately return any value, we have to detect
  // errors by checking if `errno` was set during the call.
  errno = 0;
  char* endptr;
  int base = 10;
  long long parsed = std::strtoll(idParameter->c_str(), &endptr, base);
  if (errno) {
    return Error(strerror(errno));
  }

  if (endptr != idParameter->c_str() + idParameter->size()) {
    return Error("Garbage after parsed id");
  }

  return parsed;
}

}  // namespace {


namespace process {

// Interface to interact with the `jemalloc` library.
namespace jemalloc {

Try<bool> startProfiling()
{
  return updateJemallocSetting("prof.active", true);
}


Try<bool> stopProfiling()
{
  return updateJemallocSetting("prof.active", false);
}


Try<bool> profilingActive()
{
  return readJemallocSetting<bool>("prof.active");
}


Try<Nothing> dump(const string& path)
{
  // A profile is dumped every time the 'prof.dump' setting is written to.
  return writeJemallocSetting("prof.dump", path.c_str());
}

} // namespace jemalloc {


const string MemoryProfiler::START_HELP()
{
  return HELP(
      TLDR(
          "Starts collection of stack traces."),
      DESCRIPTION(
          "Activates memory profiling.",
          "The profiling works by statistically sampling the backtraces of",
          "calls to 'malloc()'. This requires some additional memory to store",
          "the collected data. The required additional space is expected to",
          "grow logarithmically.",
          "",
          "Query parameters:",
          "",
          ">        duration=VALUE   How long to collect data before",
          ">                         stopping. (default: 5mins)"),
      AUTHENTICATION(true));
}


const string MemoryProfiler::STOP_HELP()
{
  return HELP(
      TLDR(
          "Stops memory profiling and dumps collected data."),
      DESCRIPTION(
          "Instructs the memory profiler to stop collecting data"
          "and dumps a file containing the collected data to disk,"
          "clearing that data from memory. Does nothing if profiling",
          "has not been started before."),
      AUTHENTICATION(true));
}


const string MemoryProfiler::DOWNLOAD_RAW_HELP()
{
  return HELP(
      TLDR(
          "Returns a raw memory profile."),
      DESCRIPTION(
          "Returns a file that was generated when the '/stop' endpoint",
          "was last accessed. See the jemalloc [manual page][manpage] for",
          "information about the file format.",
          "",
          "Query parameters:",
          "",
          ">        id=VALUE         Optional parameter to request a specific",
          ">                         version of the profile."),
      AUTHENTICATION(true),
      None(),
      REFERENCES("[manpage]: http://jemalloc.net/jemalloc.3.html"));
}


const string MemoryProfiler::DOWNLOAD_TEXT_HELP()
{
  return HELP(
      TLDR(
          "Generates and returns a symbolized memory profile."),
      DESCRIPTION(
          "Generates a symbolized profile.",
          "Requires that the running binary was built with symbols and that",
          "jeprof is installed on the host machine.",
          "",
          "**NOTE:** Generating the returned file might take several minutes.",
          "",
          "Query parameters:",
          ">        id=VALUE         Optional parameter to request a specific",
          ">                         version of the generated profile."),
      AUTHENTICATION(true));
}


const string MemoryProfiler::DOWNLOAD_GRAPH_HELP()
{
  return HELP(
      TLDR(
          "Generates and returns a graph visualization."),
      DESCRIPTION(
          "Generates a graphical representation of the raw profile in SVG.",
          "Using this endpoint requires that that jeprof and dot are installed",
          "on the host machine.",
          "",
          "**NOTE:** Generating the returned file might take several minutes.",
          "",
          "Query parameters:",
          "",
          ">        id=VALUE         Optional parameter to request a specific",
          ">                         version of the generated graph."),
      AUTHENTICATION(true));
}


const string MemoryProfiler::STATISTICS_HELP()
{
  return HELP(
      TLDR(
          "Shows memory allocation statistics."),
      DESCRIPTION(
          "Memory allocation statistics as returned by 'malloc_stats_print()'.",
          "These track e.g. the total number of bytes allocated by the current",
          "process and the bin-size of these allocations.",
          "These statistics are unrelated to the profiling mechanism managed",
          "by the '/start' and '/stop' endpoints, and are always accurate.",
          "",
          "Returns a JSON object."),
      AUTHENTICATION(true));
}


const string MemoryProfiler::STATE_HELP()
{
  return HELP(
      TLDR(
          "Shows the configuration of the memory profiler process."),
      DESCRIPTION(
          "Current memory profiler state. This shows, for example, whether",
          "jemalloc was detected, whether profiling is currently active and",
          "the directory used to store temporary files.",
          "",
          "Returns a JSON object."),
      AUTHENTICATION(true));
}


void MemoryProfiler::initialize()
{
  route("/start",
        authenticationRealm,
        START_HELP(),
        &MemoryProfiler::start);

  route("/stop",
        authenticationRealm,
        STOP_HELP(),
        &MemoryProfiler::stop);

  route("/download/raw",
        authenticationRealm,
        DOWNLOAD_RAW_HELP(),
        &MemoryProfiler::downloadRawProfile);

  route("/download/text",
        authenticationRealm,
        DOWNLOAD_TEXT_HELP(),
        &MemoryProfiler::downloadSymbolizedProfile);

  route("/download/graph",
        authenticationRealm,
        DOWNLOAD_GRAPH_HELP(),
        &MemoryProfiler::downloadGraphProfile);

  route("/statistics",
        authenticationRealm,
        STATISTICS_HELP(),
        &MemoryProfiler::statistics);

  route("/state",
        authenticationRealm,
        STATE_HELP(),
        &MemoryProfiler::state);
}


MemoryProfiler::ProfilingRun::ProfilingRun(
    MemoryProfiler* profiler,
    time_t id,
    const Duration& duration)
  : id(id),
    timer(delay(
        duration,
        profiler,
        &MemoryProfiler::stopAndGenerateRawProfile))
{}


void MemoryProfiler::ProfilingRun::extend(
    MemoryProfiler* profiler,
    const Duration& duration)
{
  Duration remaining = timer.timeout().remaining();
  Clock::cancel(timer);
  timer = delay(
      remaining + duration,
      profiler,
      &MemoryProfiler::stopAndGenerateRawProfile);
}


MemoryProfiler::DiskArtifact::DiskArtifact(
    const std::string& path,
    time_t id)
  : path(path),
    id(id)
{}


const time_t MemoryProfiler::DiskArtifact::getId() const
{
  return id;
}


string MemoryProfiler::DiskArtifact::getPath() const
{
  return path;
}


http::Response MemoryProfiler::DiskArtifact::asHttp() const
{
  // If we get here, we want to serve the file that *should* be on disk.
  // Verify that it still exists before attempting to serve it.
  //
  // TODO(bevers): Store a checksum and verify that it matches.
  if (!os::stat::isfile(path)) {
    return http::BadRequest("Requested file was deleted from local disk.\n");
  }

  process::http::OK response;
  response.type = response.PATH;
  response.path = path;
  response.headers["Content-Type"] = "application/octet-stream";
  response.headers["Content-Disposition"] =
    strings::format("attachment; filename=%s", path).get();

  return std::move(response);
}


Try<MemoryProfiler::DiskArtifact> MemoryProfiler::DiskArtifact::create(
    const string& filename,
    time_t timestamp,
    std::function<Try<Nothing>(const string&)> generator)
{
  Try<Path> tmpdir = getTemporaryDirectoryPath();
  if (tmpdir.isError()) {
    return Error("Could not determine target path: " + tmpdir.error());
  }

  const string path = path::join(tmpdir.get(), filename);

  Try<Nothing> result = generator(path);

  if (result.isError()) {
    // The old file might still be fine on disk, but there's no good way to
    // verify this hence we assume that the error rendered it unusable.
    return Error("Failed to create artifact: " + result.error());
  }

  return MemoryProfiler::DiskArtifact(path, timestamp);
}


MemoryProfiler::MemoryProfiler(const Option<string>& _authenticationRealm)
  : ProcessBase("memory-profiler"),
    authenticationRealm(_authenticationRealm)
{}


// TODO(bevers): Add a query parameter to select json or html format.
// TODO(bevers): Add a query parameter to configure the sampling interval.
Future<http::Response> MemoryProfiler::start(
  const http::Request& request,
  const Option<http::authentication::Principal>&)
{
  if (!detectJemalloc()) {
    return http::BadRequest(string(JEMALLOC_NOT_DETECTED_MESSAGE) + ".\n");
  }

  Duration duration = DEFAULT_COLLECTION_TIME;

  // TODO(bevers): Introduce `http::Request::extractQueryParameter<T>(string)`
  // instead of doing it ad-hoc here.
  Option<string> durationParameter = request.url.query.get("duration");
  if (durationParameter.isSome()) {
    Try<Duration> parsed = Duration::parse(durationParameter.get());
    if (parsed.isError()) {
      return http::BadRequest(
          "Could not parse parameter 'duration': " + parsed.error() + ".\n");
    }
    duration = parsed.get();
  }

  if (duration < MINIMUM_COLLECTION_TIME ||
      duration > MAXIMUM_COLLECTION_TIME) {
    return http::BadRequest(
        "Duration '" + stringify(duration) + "' must be between "
        + stringify(MINIMUM_COLLECTION_TIME) + " and "
        + stringify(MAXIMUM_COLLECTION_TIME) + ".\n");
  }

  Try<bool> wasActive = jemalloc::startProfiling();
  if (wasActive.isError()) {
    return http::BadRequest(
        string(JEMALLOC_PROFILING_NOT_ENABLED_MESSAGE) + ".\n");
  }

  if (!wasActive.get()) {
    time_t id = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    currentRun = ProfilingRun(this, id, duration);
  }

  JSON::Object response;

  // This can happen when jemalloc was configured e.g. via the `MALLOC_CONF`
  // environment variable. We don't touch it in this case.
  if (!currentRun.isSome()) {
    return http::Conflict("Heap profiling was started externally.\n");
  }

  string message = wasActive.get() ?
    "Heap profiling is already active." :
    "Successfully started new heap profiling run.";

  message +=
    " After the remaining time elapses, download the generated profile at '/" +
    this->self().id + "/download/raw?id=" + stringify(currentRun->id) + "'." +
    " Visit '/" + this->self().id + "/stop' to stop collection earlier.";

  // Adding 0.5 to round to nearest integer value.
  response.values["remaining_seconds"] = stringify(static_cast<int>(
      currentRun->timer.timeout().remaining().secs() + 0.5));
  response.values["message"] = message;
  response.values["id"] = currentRun->id;

  return http::OK(response);
}


// TODO(bevers): Add a way to dump an intermediate profile without
// stopping the data collection.
Future<http::Response> MemoryProfiler::stop(
    const http::Request& request,
    const Option<http::authentication::Principal>&)
{
  if (!detectJemalloc()) {
    return http::BadRequest(string(JEMALLOC_NOT_DETECTED_MESSAGE) + ".\n");
  }

  Try<bool> active = jemalloc::profilingActive();
  if (active.isError()) {
    return http::BadRequest(
        "Error interfacing with jemalloc: " + active.error() + ".\n");
  }

  if (!currentRun.isSome() && active.get()) {
    // TODO(bevers): Allow stopping even in this case.
    return http::BadRequest(
        "Profiling is active, but was not started by libprocess. Accessing the"
        " raw profile through libprocess is currently not supported.\n");
  }

  // If stop is successful or a no-op, `jemallocRawProfile` will be `Some`.
  stopAndGenerateRawProfile();

  if (rawProfile.isError()) {
    return http::BadRequest(rawProfile.error() + ".\n");
  }

  Try<bool> stillActive = jemalloc::profilingActive();
  CHECK(stillActive.isError() || !stillActive.get());

  const string message =
    "Successfully stopped memory profiling run."
    " Use one of the provided URLs to download results."
    " Note that in order to generate graphs or symbolized profiles,"
    " jeprof must be installed on the host machine and generation of"
    " these files can take several minutes.";

  const string id = stringify(rawProfile->getId());

  JSON::Object result;
  result.values["id"] = id;
  result.values["message"] = message;

  result.values["url_raw_profile"] =
    "/" + this->self().id + "/download/raw?id=" + id;

  result.values["url_graph_profile"] =
    "/" + this->self().id + "/download/graph?id=" + id;

  result.values["url_symbolized_profile"] =
    "/" + this->self().id + "/download/text?id=" + id;

  return http::OK(result);
}


Future<http::Response> MemoryProfiler::downloadRawProfile(
    const http::Request& request,
    const Option<http::authentication::Principal>&)
{
  Result<time_t> requestedId = extractIdFromRequest(request);

  // Verify that `id` has the correct version if it was explicitly passed.
  if (requestedId.isError()) {
    return http::BadRequest(
        "Invalid parameter 'id': " + requestedId.error() + ".\n");
  }

  if (currentRun.isSome() && !requestedId.isSome()) {
    return http::BadRequest(
        "A profiling run is currently in progress. To download results of the"
        " previous run, please pass an 'id' explicitly.\n");
  }

  if (rawProfile.isError()) {
    return http::BadRequest(
        "Cannot access raw profile: " + rawProfile.error() + ".\n");
  }

  // Only requests for the latest available version are allowed.
  if (requestedId.isSome() &&
      (requestedId.get() != rawProfile->getId())) {
    return http::BadRequest(
        "Cannot serve requested id #" + stringify(requestedId.get()) + ".\n");
  }

  return rawProfile->asHttp();
}


Future<http::Response> MemoryProfiler::downloadSymbolizedProfile(
    const http::Request& request,
    const Option<http::authentication::Principal>&)
{
  Result<time_t> requestedId = extractIdFromRequest(request);

  // Verify that `id` has the correct version if it was explicitly passed.
  if (requestedId.isError()) {
    return http::BadRequest(
        "Invalid parameter 'id': " + requestedId.error() + ".\n");
  }

  if (currentRun.isSome() && !requestedId.isSome()) {
    return http::BadRequest(
        "A profiling run is currently in progress. To download results of the"
        " previous run, please pass an 'id' explicitly.\n");
  }

  if (rawProfile.isError()) {
    return http::BadRequest(
        "No source profile exists: " + rawProfile.error() + ".\n");
  }

  const string rawProfilePath = rawProfile->getPath();
  const time_t rawProfileId = rawProfile->getId();

  // Only requests for the latest available version are allowed.
  if (requestedId.isSome() && (requestedId.get() != rawProfileId)) {
    return http::BadRequest(
        "Cannot serve requested id #" + stringify(requestedId.get()) + ".\n");
  }

  // Generate the profile for the latest available version
  // or return the cached file on disk.
  if (symbolizedProfile.isError() ||
      (symbolizedProfile->getId() != rawProfileId)) {
    symbolizedProfile = DiskArtifact::create(
        SYMBOLIZED_PROFILE_FILENAME,
        rawProfileId,
        [rawProfilePath](const string& outputPath) -> Try<Nothing> {
          return generateJeprofFile(
              rawProfilePath,
              "--text",
              outputPath);
        });
  }

  if (symbolizedProfile.isSome()) {
    return symbolizedProfile->asHttp();
  } else {
    const string message = "Cannot generate file: " + symbolizedProfile.error();
    LOG(WARNING) << message;
    return http::BadRequest(message + ".\n");
  }
}


Future<http::Response> MemoryProfiler::downloadGraphProfile(
    const http::Request& request,
    const Option<http::authentication::Principal>&)
{
  Result<time_t> requestedId = extractIdFromRequest(request);

  // Verify that `id` has the correct version if it was explicitly passed.
  if (requestedId.isError()) {
    return http::BadRequest(
        "Invalid parameter 'id': " + requestedId.error() + ".\n");
  }

  if (currentRun.isSome() && !requestedId.isSome()) {
    return http::BadRequest(
        "A profiling run is currently in progress. To download results of the"
        " previous run, please pass an 'id' explicitly.\n");
  }

  if (rawProfile.isError()) {
    return http::BadRequest(
        "No source profile exists: " + rawProfile.error() + ".\n");
  }

  const string rawProfilePath = rawProfile->getPath();
  const time_t rawProfileId = rawProfile->getId();

  // Only requests for the latest available version are allowed.
  if (requestedId.isSome() && (requestedId.get() != rawProfileId)) {
    return http::BadRequest(
        "Cannot serve requested id #" + stringify(requestedId.get()) + ".\n");
  }

  // Generate the profile for the latest available version
  // or return the cached file on disk.
  if (graphProfile.isError() || (graphProfile->getId() != rawProfileId)) {
    graphProfile = DiskArtifact::create(
        GRAPH_FILENAME,
        rawProfileId,
        [rawProfilePath](const string& outputPath) -> Try<Nothing> {
          return generateJeprofFile(
              rawProfilePath,
              "--svg",
              outputPath);
        });
  }

  if (graphProfile.isSome()) {
    return graphProfile->asHttp();
  } else {
    const string message = "Cannot generate file: " + graphProfile.error();
    LOG(WARNING) << message;
    return http::BadRequest(message + ".\n");
  }
}


// TODO(bevers): Allow passing custom options via query parameters.
Future<http::Response> MemoryProfiler::statistics(
    const http::Request& request,
    const Option<http::authentication::Principal>&)
{
  if (!detectJemalloc()) {
    return http::BadRequest(string(JEMALLOC_NOT_DETECTED_MESSAGE) + ".\n");
  }

  const string options = "J";  // 'J' selects JSON output format.

  string statistics;

#ifdef LIBPROCESS_ALLOW_JEMALLOC
  ::malloc_stats_print([](void* opaque, const char* msg) {
      string* statistics = static_cast<string*>(opaque);
      *statistics += msg;
    }, &statistics, options.c_str());
#endif

  return http::OK(statistics, "application/json; charset=utf-8");
}


Future<http::Response> MemoryProfiler::state(
  const http::Request& request,
  const Option<http::authentication::Principal>&)
{
  bool detected = detectJemalloc();

  JSON::Object state;

  {
    // State unrelated to jemalloc.
    JSON::Object profilerState;
    profilerState.values["jemalloc_detected"] = detected;

    profilerState.values["tmp_dir"] = stringify(
        temporaryDirectory.getOrElse("Not yet generated"));

    {
      JSON::Object runInformation;
      if (currentRun.isSome()) {
        runInformation.values["id"] = currentRun->id;
        runInformation.values["remaining_seconds"] =
          currentRun->timer.timeout().remaining().secs();
      } else if (rawProfile.isSome()) {
        runInformation.values["id"] = rawProfile->getId();
        runInformation.values["remaining_seconds"] = 0;
      } else {
        runInformation.values["id"] = JSON::Null();
      }

      profilerState.values["current_run"] = std::move(runInformation);
    }

    state.values["memory_profiler"] = std::move(profilerState);
  }

  if (!detected) {
    return http::OK(state);
  }

  {
    // Holds relevant parts of the current jemalloc state.
    JSON::Object jemallocState;

    {
      // Holds malloc configuration from various sources.
      JSON::Object mallocConf;

      // User-specified malloc configuration that was added via
      // the `MALLOC_CONF` environment variable.
      mallocConf.values["environment"] =
        os::getenv("MALLOC_CONF").getOrElse("");

      // Compile-time malloc configuration that was added at build time via
      // the `--with-malloc-conf` flag.
      Try<const char*> builtinMallocConf = readJemallocSetting<const char*>(
          "config.malloc_conf");

      if (builtinMallocConf.isError()) {
        mallocConf.values["build_options"] = builtinMallocConf.error();
      } else {
        mallocConf.values["build_options"] = builtinMallocConf.get();
      }

      // TODO(bevers): System-wide jemalloc settings can be specified by
      // creating a symlink at /etc/malloc.conf whose pointed-to value is read
      // as an option string.
      // Application-specific jemalloc settings can be specified by creating
      // an externally visible symbol called `malloc_conf`.
      // We should also display both of these here.

      jemallocState.values["malloc_conf"] = std::move(mallocConf);
    }

    // Whether jemalloc was compiled with support for heap profiling.
    Try<bool> profilingSupported = readJemallocSetting<bool>("config.prof");

    if (profilingSupported.isError()) {
      jemallocState.values["profiling_enabled"] = profilingSupported.error();
    } else {
      jemallocState.values["profiling_enabled"] = profilingSupported.get();
    }

    // Whether profiling is currently active.
    Try<bool> profilingActive = readJemallocSetting<bool>("prof.active");

    if (profilingActive.isError()) {
      jemallocState.values["profiling_active"] = profilingActive.error();
    } else {
      jemallocState.values["profiling_active"] = profilingActive.get();
    }

    state.values["jemalloc"] = std::move(jemallocState);
  }

  return http::OK(state);
}


void MemoryProfiler::stopAndGenerateRawProfile()
{
  ASSERT(detectJemalloc());

  VLOG(1) << "Attempting to stop current profiling run";

  // If there is no current profiling run, there is nothing to do.
  if (!currentRun.isSome()) {
    return;
  }

  Try<bool> stopped = jemalloc::stopProfiling();

  if (stopped.isError()) {
    LOG(WARNING) << "Failed to stop memory profiling: " << stopped.error();

    // Don't give up. Probably it will fail again in the future, but at least
    // the problem will be clearly visible in the logs.
    currentRun->extend(this, Seconds(5));

    return;
  }

  // Heap profiling should not be active any more.
  // We won't retry stopping and generating a profile after this point:
  // We're not actively sampling any more, and if the user still cares
  // about this profile they will get the data with the next run.
  Try<bool> stillActive = jemalloc::profilingActive();
  CHECK(stillActive.isError() || !stillActive.get());

  time_t runId = currentRun->id;
  Clock::cancel(currentRun->timer);
  currentRun = None();

  if (!stopped.get()) {
    // This is a weird state to end up in, apparently something else in this
    // process stopped profiling independently of us.
    // If there was some valuable, un-dumped data it is still possible to get
    // it by starting a new run.
    LOG(WARNING)
      << "Memory profiling unexpectedly inactive; not dumping profile. Ensure"
      << " nothing else is interfacing with jemalloc in this process";
    return;
  }

  // We store the new artifact even in case of error to surface it to the user.
  rawProfile = DiskArtifact::create(
      RAW_PROFILE_FILENAME,
      runId,
      [](const string& outputPath) -> Try<Nothing> {
        // Make sure we actually have permissions to write to the file and that
        // there is at least a little bit space left on the device.
        const string data(DUMMY_FILE_SIZE, '\0');
        Try<Nothing> written = os::write(outputPath, data);
        if (written.isError()) {
          return Error(written.error());
        }

        // Verify independently that the file was actually written.
        Try<Bytes> size = os::stat::size(outputPath);
        if (size.isError() || size.get() != DUMMY_FILE_SIZE) {
          return Error(strings::format(
              "Couldn't verify integrity of dump file %s", outputPath).get());
        }

        // Finally, do the real dump.
        return jemalloc::dump(outputPath);
      });

  if (rawProfile.isError()) {
    LOG(WARNING) << "Cannot dump profile: " + rawProfile.error();
  }
}

} // namespace process {
