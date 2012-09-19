#include <sys/stat.h>

#include <map>
#include <string>
#include <vector>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "files/files.hpp"

#include "logging/logging.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using process::http::BadRequest;
using process::http::InternalServerError;
using process::http::NotFound;
using process::http::OK;
using process::http::Response;
using process::http::Request;

using std::map;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

class FilesProcess : public Process<FilesProcess>
{
public:
  FilesProcess();

  // Files implementation.
  Future<Nothing> attach(const string& path, const string& name);
  void detach(const string& name);

protected:
  virtual void initialize();

private:
  // HTTP endpoints.
  Future<Response> browse(const Request& request);
  Future<Response> read(const Request& request);
  Future<Response> debug(const Request& request);

  // Resolves the virtual path to an actual path.
  // Returns the actual path if found.
  // Returns None if the file is not found.
  // Returns Error if we find the file but it cannot be resolved or it breaks
  // out of the chroot.
  Result<std::string> resolve(const string& path);

  hashmap<string, string> paths;
};


FilesProcess::FilesProcess()
  : ProcessBase("files")
{}


void FilesProcess::initialize()
{
  route("/browse.json", &FilesProcess::browse);
  route("/read.json", &FilesProcess::read);
  route("/debug.json", &FilesProcess::debug);
}


Future<Nothing> FilesProcess::attach(const string& path, const string& name)
{
  Try<string> result = os::realpath(path);

  if (result.isError()) {
    LOG(ERROR) << "Error attaching path '" << path << "': " << result.error();
    return Future<Nothing>::failed(result.error());
  }

  // Make sure we have permissions to read the file/dir.
  Try<bool> access = os::access(result.get(), R_OK);

  if (access.isError()) {
    LOG(WARNING) << "Error attaching path '" << path << "': " << access.error();
    return Future<Nothing>::failed(access.error());
  } else if (!access.get()) {
    LOG(WARNING) << "Do not have read permission to attach path: " << path;
    return Future<Nothing>::failed(access.error());
  }

  // To simplify the read/browse logic, strip any trailing / from the name.
  string cleanedName = strings::remove(name, "/", strings::SUFFIX);

  // TODO(bmahler): Do we want to always wipe out the previous path?
  paths[cleanedName] = result.get();

  return Nothing();
}


void FilesProcess::detach(const string& name)
{
  paths.erase(name);
}


Future<Response> FilesProcess::browse(const Request& request)
{
  Option<string> path = request.query.get("path");

  if (!path.isSome() || path.get().empty()) {
    return BadRequest("Expecting 'path=value' in query.\n");
  }

  Result<string> resolvedPath = resolve(path.get());

  if (resolvedPath.isError()) {
    return InternalServerError(resolvedPath.error());
  } else if (resolvedPath.isNone()) {
    return NotFound();
  }

  // The result will be a sorted (on path) array of files and dirs:
  // [{"name": "README", "path": "dir/README" "dir":False, "size":42}, ...]
  map<string, JSON::Object> files;
  foreach (const string& filename, os::ls(resolvedPath.get())) {
    struct stat s;
    string fullPath = path::join(resolvedPath.get(), filename);

    if (stat(fullPath.c_str(), &s) < 0) {
      PLOG(WARNING) << "Found " << fullPath << " in ls but stat failed";
      continue;
    }

    files[fullPath] = jsonFileInfo(path::join(path.get(), filename),
                                   S_ISDIR(s.st_mode),
                                   S_ISDIR(s.st_mode) ? 0 : s.st_size);
  }

  JSON::Array listing;
  foreachvalue(const JSON::Object& file, files) {
    listing.values.push_back(file);
  }

  return OK(listing, request.query.get("jsonp"));
}


Future<Response> FilesProcess::read(const Request& request)
{
  Option<string> path = request.query.get("path");

  if (!path.isSome() || path.get().empty()) {
    return BadRequest("Expecting 'path=value' in query.\n");
  }

  off_t offset = -1;

  if (request.query.get("offset").isSome()) {
    Try<off_t> result = numify<off_t>(request.query.get("offset").get());
    if (result.isError()) {
      return BadRequest("Failed to parse offset: " + result.error());
    }
    offset = result.get();
  }

  ssize_t length = -1;

  if (request.query.get("length").isSome()) {
    Try<ssize_t> result = numify<ssize_t>(request.query.get("length").get());
    if (result.isError()) {
      return BadRequest("Failed to parse length: " + result.error());
    }
    length = result.get();
  }

  Result<string> resolvedPath = resolve(path.get());

  if (resolvedPath.isError()) {
    return BadRequest(resolvedPath.error());
  } else if (!resolvedPath.isSome()) {
    return NotFound();
  }

  // Don't read directories.
  if (os::isdir(resolvedPath.get())) {
    return BadRequest("Cannot read a directory.");
  }

  // TODO(benh): Cache file descriptors so we aren't constantly
  // opening them and paging the data in from disk.
  Try<int> fd = os::open(resolvedPath.get(), O_RDONLY);

  if (fd.isError()) {
    string error = strings::format("Failed to open file at '%s': %s",
        resolvedPath.get(), fd.error()).get();
    LOG(WARNING) << error;
    return InternalServerError(error);
  }

  off_t size = lseek(fd.get(), 0, SEEK_END);

  if (size == -1) {
    string error = strings::format("Failed to open file at '%s': %s",
        resolvedPath.get(), strerror(errno)).get();
    LOG(WARNING) << error;
    close(fd.get());
    return InternalServerError(error);
  }

  if (offset == -1) {
    offset = size;
  }

  if (length == -1) {
    length = size - offset;
  }

  JSON::Object object;

  if (offset < size) {
    // Seek to the offset we want to read from.
    if (lseek(fd.get(), offset, SEEK_SET) == -1) {
      string error = strings::format("Failed to seek file at '%s': %s",
          resolvedPath.get(), strerror(errno)).get();
      LOG(WARNING) << error;
      close(fd.get());
      return InternalServerError(error);
    }

    // Read length bytes (or to EOF).
    char* temp = new char[length];

    // TODO(bmahler): Change this to use async process::read.
    length = ::read(fd.get(), temp, length);

    if (length == 0) {
      object.values["offset"] = offset;
      object.values["length"] = 0;
      delete[] temp;
    } else if (length == -1) {
      string error = strings::format("Failed to read file at '%s': %s",
          resolvedPath.get(), strerror(errno)).get();
      LOG(WARNING) << error;
      delete[] temp;
      close(fd.get());
      return InternalServerError(error);
    } else {
      object.values["offset"] = offset;
      object.values["length"] = length;
      object.values["data"] = string(temp, length);
      delete[] temp;
    }
  } else {
    object.values["offset"] = size;
    object.values["length"] = 0;
  }

  close(fd.get());

  return OK(object, request.query.get("jsonp"));
}


Future<Response> FilesProcess::debug(const Request& request)
{
  JSON::Object object;
  foreachpair(const string& name, const string& path, paths) {
    object.values[name] = path;
  }
  return OK(object, request.query.get("jsonp"));
}


Result<string> FilesProcess::resolve(const string& path)
{
  // Suppose we have: /1/2/hello_world.txt
  // And we attach: /1/2 as /sandbox
  // Then this function would resolve the following virtual path
  // into the actual path:
  // input: /sandbox/hello_world.txt
  // output: /1/2/hello_world.txt
  //
  // Try and see if this path has been attached. We check for the
  // longest possible prefix match and if found append any suffix to
  // the attached path (provided the path is to a directory).
  vector<string> tokens = strings::split(
      strings::remove(path, "/", strings::SUFFIX), "/");

  string suffix;
  while (!tokens.empty()) {
    string prefix = path::join(tokens);

    if (!paths.contains(prefix)) {
      if (suffix.empty()) {
        suffix = tokens.back();
      } else {
        suffix = path::join(tokens.back(), suffix);
      }

      tokens.pop_back();
      continue;
    }

    // Determine the final path: if it's a directory, append the
    // suffix, if it's not a directory and there is a suffix, return
    // 'Not Found'.
    string path = paths[prefix];
    if (os::isdir(path)) {
      path = path::join(path, suffix);

      // Canonicalize the absolute path and make sure the result is
      // accessible.
      Try<string> result = os::realpath(path);
      if (result.isError()) {
        return Result<string>::error("Cannot resolve path.");
      } else if (!strings::startsWith(result.get(), paths[prefix])) {
        return Result<string>::error("Resolved path is inaccessible.");
      }

      path = result.get();
    } else if (suffix != "") {
      // Request is assuming attached path is a directory, but it is
      // not! Rather than 'Bad Request', treat this as 'Not Found'.
      return Result<string>::none();
    }

    return path;
  }

  return Result<string>::none();
}


Files::Files()
{
  process = new FilesProcess();
  spawn(process);
}


Files::~Files()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Nothing> Files::attach(const string& path, const string& name)
{
  return dispatch(process, &FilesProcess::attach, path, name);
}


void Files::detach(const string& name)
{
  dispatch(process, &FilesProcess::detach, name);
}


PID<> Files::pid()
{
  return process->self();
}

} // namespace internal {
} // namespace mesos {
