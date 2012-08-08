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
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "files/files.hpp"

#include "logging/logging.hpp"

using namespace process;

using process::http::BadRequest;
using process::http::InternalServerError;
using process::http::NotFound;
using process::http::OK;
using process::http::Response;
using process::http::Request;
using process::http::ServiceUnavailable;

using std::map;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

class FilesProcess : public Process<FilesProcess>
{
public:
  FilesProcess() {}
  virtual ~FilesProcess() {}

  // Files implementation.
  Future<bool> attach(const string& path, const string& name);
  void detach(const string& name);

protected:
  virtual void initialize();

private:
  // JSON endpoints.
  Future<Response> browse(const Request& request);
  Future<Response> read(const Request& request);

  hashmap<string, string> paths;
};


void FilesProcess::initialize()
{
  route("/browse.json", &FilesProcess::browse);
  route("/read.json", &FilesProcess::read);
}


Future<bool> FilesProcess::attach(const string& path, const string& name)
{
  Try<string> result = os::realpath(path);

  if (result.isError()) {
    return Future<bool>::failed(result.error());
  }

  // Make sure we have permissions to "touch" the file (TODO(benh): We
  // really only need permissions to read the file).
  Try<bool> touched = os::touch(result.get());

  if (touched.isError()) {
    return Future<bool>::failed(touched.error());
  }

  if (touched.get()) {
    paths[name] = result.get();
  }

  return touched.get();
}


void FilesProcess::detach(const string& name)
{
  paths.erase(name);
}


Future<Response> FilesProcess::browse(const Request& request)
{
  return ServiceUnavailable();
}


Future<Response> FilesProcess::read(const Request& request)
{
  map<string, vector<string> > pairs =
    strings::pairs(request.query, ";&", "=");

  Option<string> name;

  if (pairs.count("name") > 0 && pairs["name"].size() > 0) {
    name = pairs["name"].back();
  }

  if (!name.isSome()) {
    return BadRequest();
  }

  off_t offset = -1;

  if (pairs.count("offset") > 0 && pairs["offset"].size() > 0) {
    Try<off_t> result = numify<off_t>(pairs["offset"].back());
    if (result.isError()) {
      LOG(WARNING) << "Failed to \"numify\" the 'offset' ("
                   << pairs["offset"].back() << "): "
                   << result.error();
      return InternalServerError();
    }
    offset = result.get();
  }

  ssize_t length = -1;

  if (pairs.count("length") > 0) {
    CHECK(pairs["length"].size() > 0);
    Try<ssize_t> result = numify<ssize_t>(pairs["length"].back());
    if (result.isError()) {
      LOG(WARNING) << "Failed to \"numify\" the 'length' ("
                   << pairs["length"].back() << "): "
                   << result.error();
      return InternalServerError();
    }
    length = result.get();
  }

  // Now try and see if this name has been attached. We check for the
  // longest possible prefix match and if found append any suffix to
  // the attached path (provided the path is to a directory).
  string suffix;

  foreach (const string& s, strings::split(name.get(), "/")) {
    string prefix = name.get().substr(0, name.get().length() - suffix.length());

    if (!paths.contains(prefix)) {
      suffix = s + "/" + suffix;
    } else {
      // Determine the final path: if it's a directory, append the
      // suffix, if it's not a directory and there is a suffix, return
      // '404 Not Found'.
      string path;
      if (os::exists(paths[prefix], true)) {
        path = paths[prefix] + "/" + suffix;

        // Canonicalize the absolute path and make sure the result
        // doesn't break out of the chroot (i.e., resolving any '..'
        // in the suffix should yield a resulting path that still
        // contains the attached path as it's prefix).
        Try<string> result = os::realpath(path);
        if (result.isError()) {
          return NotFound();
        } else if (result.get().find(paths[prefix]) != 0) {
          return NotFound();
        }

        path = result.get();
      } else if (suffix != "") {
        return NotFound();
      }

      // TODO(benh): Cache file descriptors so we aren't constantly
      // opening them and paging the data in from disk.
      Try<int> fd = os::open(path, O_RDONLY);

      if (fd.isError()) {
        LOG(WARNING) << "Failed to open file at "
                     << path << ": " << fd.error();
        return InternalServerError();
      }

      off_t size = lseek(fd.get(), 0, SEEK_END);

      if (size == -1) {
        PLOG(WARNING) << "Failed to seek in the file at " << path;
        close(fd.get());
        return InternalServerError();
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
          PLOG(WARNING) << "Failed to seek in the file at " << path;
          close(fd.get());
          return InternalServerError();
        }

        // Read length bytes (or to EOF).
        char* temp = new char[length];

        length = ::read(fd.get(), temp, length);

        if (length == 0) {
          object.values["offset"] = offset;
          object.values["length"] = 0;
          delete[] temp;
        } else if (length == -1) {
          PLOG(WARNING) << "Failed to read from the file at " << path;
          delete[] temp;
          close(fd.get());
          return InternalServerError();
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

      std::ostringstream out;

      JSON::render(out, object);

      OK response;
      response.headers["Content-Type"] = "application/json";
      response.headers["Content-Length"] = stringify(out.str().size());
      response.body = out.str().data();
      return response;
    }
  }

  return NotFound();
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
}


Future<bool> Files::attach(const string& path, const string& name)
{
  return dispatch(process, &FilesProcess::attach, path, name);
}


void Files::detach(const string& name)
{
  dispatch(process, &FilesProcess::detach, name);
}

} // namespace internal {
} // namespace mesos {
