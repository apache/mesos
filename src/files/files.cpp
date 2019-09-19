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
// limitations under the License

#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <boost/shared_array.hpp>

#include <process/defer.hpp>
#include <process/deferred.hpp> // TODO(benh): This is required by Clang.
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/mime.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/realpath.hpp>

#include "common/http.hpp"

#include "files/files.hpp"

#include "logging/logging.hpp"

namespace http = process::http;
namespace io = process::io;
namespace mime = process::mime;

using http::BadRequest;
using http::Forbidden;
using http::InternalServerError;
using http::NotFound;
using http::OK;

using mesos::Authorizer;

using process::AUTHENTICATION;
using process::AUTHORIZATION;
using process::defer;
using process::DESCRIPTION;
using process::Failure;
using process::Future;
using process::HELP;
using process::Process;
using process::TLDR;
using process::wait; // Necessary on some OS's to disambiguate.

using process::http::authentication::Principal;

using std::list;
using std::map;
using std::string;
using std::tuple;
using std::vector;

namespace mesos {
namespace internal {

class FilesProcess : public Process<FilesProcess>
{
public:
  FilesProcess(const Option<string>& _authenticationRealm,
               const Option<Authorizer*>& _authorizer);

  // Files implementation.
  Future<Nothing> attach(
      const string& path,
      const string& virtualPath,
      const Option<lambda::function<Future<bool>(
          const Option<Principal>&)>>& authorized);

  void detach(const string& virtualPath);

  Future<Try<list<FileInfo>, FilesError>> browse(
      const string& path,
      const Option<Principal>& principal);

  Future<Try<tuple<size_t, string>, FilesError>> read(
      const size_t offset,
      const Option<size_t>& length,
      const string& path,
      const Option<Principal>& principal);

protected:
  void initialize() override;

private:
  // Resolves the virtual path to an actual path.
  // Returns the actual path if found.
  // Returns None if the file is not found.
  // Returns Error if we find the file but it cannot be resolved or it breaks
  // out of the chroot.
  Result<string> resolve(const string& path);

  Future<bool> authorize(
      string requestedPath,
      const Option<Principal>& principal);

  // HTTP endpoints.

  // Returns a file listing for a directory.
  // Requests have the following parameters:
  //   path: The directory to browse. Required.
  // The response will contain a list of JSON files and directories contained
  // in the path (see `FileInfo` model override for the format).
  Future<http::Response> _browse(
      const http::Request& request,
      const Option<Principal>& principal);

  // Continuation of `read()`.
  Future<Try<tuple<size_t, string>, FilesError>> _read(
      size_t offset,
      Option<size_t> length,
      const string& path);

  // Reads data from a file at a given offset and for a given length.
  // See the jquery pailer for the expected behavior.
  Future<http::Response> __read(
      const http::Request& request,
      const Option<Principal>& principal);

  // Returns the raw file contents for a given path.
  // Requests have the following parameters:
  //   path: The directory to browse. Required.
  Future<http::Response> download(
      const http::Request& request,
      const Option<Principal>& principal);

  Future<http::Response> _download(const string& path);

  // Returns the internal virtual path mapping.
  Future<http::Response> debug(
      const http::Request& request,
      const Option<Principal>& principal);

  // These functions log the request before continuing to the actual function.
  Future<http::Response> loggedBrowse(
      const http::Request& request,
      const Option<Principal>& principal);

  Future<http::Response> loggedRead(
      const http::Request& request,
      const Option<Principal>& principal);

  Future<http::Response> loggedDownload(
      const http::Request& request,
      const Option<Principal>& principal);

  Future<http::Response> loggedDebug(
      const http::Request& request,
      const Option<Principal>& principal);

  const static string BROWSE_HELP;
  const static string READ_HELP;
  const static string DOWNLOAD_HELP;
  const static string DEBUG_HELP;

  hashmap<string, string> paths;

  // Set of authorization functions. They will be called whenever
  // access to the path used as key is requested, and will pass
  // as parameter the principal returned by the HTTP authenticator.
  hashmap<string, lambda::function<Future<bool>(const Option<Principal>&)>>
      authorizations;

  // The authentication realm, if any, into which this process'
  // endpoints will be installed.
  Option<string> authenticationRealm;

  // FilesProcess needs an authorizer object to add authorization in
  // `/files/debug` endpoint.
  Option<Authorizer*> authorizer;
};


FilesProcess::FilesProcess(
    const Option<string>& _authenticationRealm,
    const Option<Authorizer*>& _authorizer)
  : ProcessBase("files"),
    authenticationRealm(_authenticationRealm),
    authorizer(_authorizer) {}


void FilesProcess::initialize()
{
    route("/browse",
          authenticationRealm,
          FilesProcess::BROWSE_HELP,
          &FilesProcess::loggedBrowse);
    route("/read",
          authenticationRealm,
          FilesProcess::READ_HELP,
          &FilesProcess::loggedRead);
    route("/download",
          authenticationRealm,
          FilesProcess::DOWNLOAD_HELP,
          &FilesProcess::loggedDownload);
    route("/debug",
          authenticationRealm,
          FilesProcess::DEBUG_HELP,
          &FilesProcess::loggedDebug);
}


Future<http::Response> FilesProcess::loggedBrowse(
    const http::Request& request,
    const Option<Principal>& principal)
{
  logRequest(request);
  return _browse(request, principal);
}


Future<http::Response> FilesProcess::loggedRead(
    const http::Request& request,
    const Option<Principal>& principal)
{
  logRequest(request);
  return __read(request, principal);
}


Future<http::Response> FilesProcess::loggedDownload(
    const http::Request& request,
    const Option<Principal>& principal)
{
  logRequest(request);
  return download(request, principal);
}


Future<http::Response> FilesProcess::loggedDebug(
    const http::Request& request,
    const Option<Principal>& principal)
{
  logRequest(request);
  return debug(request, principal);
}


Future<Nothing> FilesProcess::attach(
    const string& path,
    const string& virtualPath,
    const Option<lambda::function<Future<bool>(const Option<Principal>&)>>&
        authorized)
{
  const string convertedPath = path::from_uri(path);
  Result<string> result = os::realpath(convertedPath);

  if (!result.isSome()) {
    return Failure(
        "Failed to get realpath of '" + convertedPath + "': " +
        (result.isError()
         ? result.error()
         : "No such file or directory"));
  }

  // Make sure we have permissions to read the file/dir.
  Try<bool> access = os::access(result.get(), R_OK);

  if (access.isError() || !access.get()) {
    return Failure("Failed to access '" + convertedPath + "': " +
                   (access.isError() ? access.error() : "Access denied"));
  }

  // To simplify the read/browse logic, strip any trailing / from the virtual
  // path.
  string cleanedVirtualPath =
    strings::remove(path::from_uri(virtualPath),
                    stringify(os::PATH_SEPARATOR), strings::SUFFIX);

  // TODO(bmahler): Do we want to always wipe out the previous path?
  paths[cleanedVirtualPath] = result.get();

  if (authorized.isSome()) {
    authorizations[cleanedVirtualPath] = authorized.get();
  }

  return Nothing();
}


void FilesProcess::detach(const string& virtualPath)
{
  const string convertedVirtualPath = path::from_uri(virtualPath);
  paths.erase(convertedVirtualPath);
  authorizations.erase(convertedVirtualPath);
}


const string FilesProcess::BROWSE_HELP = HELP(
    TLDR(
        "Returns a file listing for a directory."),
    DESCRIPTION(
        "Lists files and directories contained in the path as",
        "a JSON object.",
        "",
        "Query parameters:",
        "",
        ">        path=VALUE          The path of directory to browse."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Browsing files requires that the request principal is",
        "authorized to do so for the target virtual file path.",
        "",
        "Authorizers may categorize different virtual paths into",
        "different ACLs, e.g. logs in one and task sandboxes in",
        "another.",
        "",
        "See authorization documentation for details."));


Future<bool> FilesProcess::authorize(
    string requestedPath,
    const Option<Principal>& principal)
{
  // The path may contain a trailing forward slash. Since we store the
  // authorization callbacks without the trailing slash, we must remove it here,
  // if present.
  const string trimmedPath =
      strings::remove(requestedPath,
                      stringify(os::PATH_SEPARATOR), strings::SUFFIX);

  if (authorizations.count(trimmedPath) > 0) {
    return authorizations[trimmedPath](principal);
  }

  do {
    requestedPath = Path(requestedPath).dirname();

    if (authorizations.count(requestedPath) > 0) {
      return authorizations[requestedPath](principal);
    }
  } while (Path(requestedPath).dirname() != requestedPath);

  // requestedPath does not require authorization.
  return true;
}


Future<http::Response> FilesProcess::_browse(
    const http::Request& request,
    const Option<Principal>& principal)
{
  Option<string> path = request.url.query.get("path");

  if (!path.isSome() || path->empty()) {
    return BadRequest("Expecting 'path=value' in query.\n");
  }

  string requestedPath = path.get();
  Option<string> jsonp = request.url.query.get("jsonp");

  return browse(requestedPath, principal)
    .then([jsonp](const Try<list<FileInfo>, FilesError>& result)
      -> Future<http::Response> {
      if (result.isError()) {
        const FilesError& error = result.error();

        switch (error.type) {
          case FilesError::Type::INVALID:
            return BadRequest(error.message);

          case FilesError::Type::NOT_FOUND:
            return NotFound(error.message);

          case FilesError::Type::UNAUTHORIZED:
            return Forbidden(error.message);

          case FilesError::Type::UNKNOWN:
            return InternalServerError(error.message);
        }

        UNREACHABLE();
      }

      JSON::Array listing;
      foreach (const FileInfo& fileInfo, result.get()) {
        listing.values.push_back(model(fileInfo));
      }

      return OK(listing, jsonp);
    });
}


Future<Try<list<FileInfo>, FilesError>> FilesProcess::browse(
    const string& path,
    const Option<Principal>& principal)
{
  const string convertedPath = path::from_uri(path);
  return authorize(convertedPath, principal)
    .then(defer(self(),
        [this, convertedPath](bool authorized)
          -> Future<Try<list<FileInfo>, FilesError>> {
      if (!authorized) {
        return FilesError(FilesError::Type::UNAUTHORIZED);
      }

      Result<string> resolvedPath = resolve(convertedPath);

      if (resolvedPath.isError()) {
        return FilesError(
            FilesError::Type::INVALID,
            resolvedPath.error() + ".\n");
      } else if (resolvedPath.isNone()) {
        return FilesError(FilesError::Type::NOT_FOUND);
      }

      // The result will be a sorted (on convertedPath) list of files and dirs.
      map<string, FileInfo> files;
      Try<list<string>> entries = os::ls(resolvedPath.get());
      if (entries.isSome()) {
        foreach (const string& entry, entries.get()) {
          struct stat s;
          string fullPath = path::join(resolvedPath.get(), entry);

          if (stat(fullPath.c_str(), &s) < 0) {
            PLOG(WARNING) << "Found " << fullPath << " in ls but stat failed";
            continue;
          }

          files[fullPath] =
            protobuf::createFileInfo(path::join(convertedPath, entry), s);
        }
      }

      list<FileInfo> listing;
      foreachvalue (const FileInfo& fileInfo, files) {
        listing.push_back(fileInfo);
      }

      return listing;
    }));
}


const string FilesProcess::READ_HELP = HELP(
    TLDR(
        "Reads data from a file."),
    DESCRIPTION(
        "This endpoint reads data from a file at a given offset and for",
        "a given length."
        "",
        "Query parameters:",
        "",
        ">        path=VALUE          The path of directory to browse.",
        ">        offset=VALUE        Value added to base address to obtain "
        "a second address",
        ">        length=VALUE        Length of file to read."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Reading files requires that the request principal is",
        "authorized to do so for the target virtual file path.",
        "",
        "Authorizers may categorize different virtual paths into",
        "different ACLs, e.g. logs in one and task sandboxes in",
        "another.",
        "",
        "See authorization documentation for details."));


Future<http::Response> FilesProcess::__read(
    const http::Request& request,
    const Option<Principal>& principal)
{
  Option<string> path = request.url.query.get("path");

  if (!path.isSome() || path->empty()) {
    return BadRequest("Expecting 'path=value' in query.\n");
  }

  off_t offset = -1;

  if (request.url.query.contains("offset")) {
    Try<off_t> result = numify<off_t>(request.url.query.at("offset"));

    if (result.isError()) {
      return BadRequest("Failed to parse offset: " + result.error() + ".\n");
    }

    if (result.get() < -1) {
      return BadRequest(strings::format(
          "Negative offset provided: %d.\n", result.get()).get());
    }

    offset = result.get();
  }

  Option<size_t> length;

  if (request.url.query.contains("length")) {
    Try<ssize_t> result = numify<ssize_t>(
        request.url.query.at("length"));

    if (result.isError()) {
      return BadRequest("Failed to parse length: " + result.error() + ".\n");
    }

    // TODO(tomxing): The pailer in the webui sends `length=-1` at first to
    // determine the length of the file, so we allow a length of -1.
    // Setting `length=-1` has the same effect as not providing a length: we
    // read to the end of the file, up to the maximum read length.
    // Will change this logic in MESOS-5334.
    if (result.get() < -1) {
      return BadRequest(strings::format(
          "Negative length provided: %d.\n", result.get()).get());
    }

    if (result.get() > -1){
      length = result.get();
    }
  }

  size_t offset_ = offset;

  // The pailer in the webui sends `offset=-1` initially to determine the length
  // of the file. This is equivalent to making a call to `read()` with an
  // `offset`/`length` of 0.
  if (offset == -1) {
    offset_ = 0;
    length = 0;
  }

  Option<string> jsonp = request.url.query.get("jsonp");

  return read(offset_, length, path.get(), principal)
    .then([offset, jsonp](const Try<tuple<size_t, string>, FilesError>& result)
        -> Future<http::Response> {
      if (result.isError()) {
        const FilesError& error = result.error();

        switch (error.type) {
          case FilesError::Type::INVALID:
            return BadRequest(error.message);

          case FilesError::Type::NOT_FOUND:
            return NotFound(error.message);

          case FilesError::Type::UNAUTHORIZED:
            return Forbidden(error.message);

          case FilesError::Type::UNKNOWN:
            return InternalServerError(error.message);
        }

        UNREACHABLE();
      }

      const tuple<size_t, string>& contents = result.get();

      JSON::Object object;
      object.values["offset"] = (offset == -1) ? std::get<0>(contents) : offset;
      object.values["data"] = std::get<1>(contents);

      return OK(object, jsonp);
    });
}


Future<Try<tuple<size_t, string>, FilesError>> FilesProcess::read(
    const size_t offset,
    const Option<size_t>& length,
    const string& path,
    const Option<Principal>& principal)
{
  const string convertedPath = path::from_uri(path);
  return authorize(convertedPath, principal)
    .then(defer(self(),
        [this, offset, length, convertedPath](bool authorized)
          -> Future<Try<tuple<size_t, string>, FilesError>> {
      if (!authorized) {
        return FilesError(FilesError::Type::UNAUTHORIZED);
      }

      return _read(offset, length, convertedPath);
    }));
}


Future<Try<tuple<size_t, string>, FilesError>> FilesProcess::_read(
    size_t offset,
    Option<size_t> length,
    const string& path)
{
  Result<string> resolvedPath = resolve(path);

  if (resolvedPath.isError()) {
    return FilesError(FilesError::Type::INVALID, resolvedPath.error() + ".\n");
  } else if (!resolvedPath.isSome()) {
    return FilesError(FilesError::Type::NOT_FOUND);
  }

  // Don't read directories.
  if (os::stat::isdir(resolvedPath.get())) {
    return FilesError(FilesError::Type::INVALID, "Cannot read a directory.\n");
  }

  // TODO(benh): Cache file descriptors so we aren't constantly
  // opening them and paging the data in from disk.
  Try<int_fd> fd = os::open(resolvedPath.get(), O_RDONLY | O_CLOEXEC);
  if (fd.isError()) {
    string error = strings::format(
        "Failed to open file at '%s': %s",
        resolvedPath.get(),
        fd.error()).get();
    LOG(WARNING) << error;
    return FilesError(FilesError::Type::UNKNOWN, error + ".\n");
  }

  Try<off_t> lseek = os::lseek(fd.get(), 0, SEEK_END);
  if (lseek.isError()) {
    string error = strings::format(
        "Failed to open file at '%s': %s",
        resolvedPath.get(),
        os::strerror(errno)).get();

    LOG(WARNING) << error;
    os::close(fd.get());
    return FilesError(FilesError::Type::UNKNOWN, error + ".\n");
  }

  const off_t size = lseek.get();

  if (offset >= static_cast<size_t>(size)) {
    os::close(fd.get());
    return std::make_tuple(size, "");
  }

  if (length.isNone()) {
    length = size - offset;
  }

  // Return the size of file if length is 0.
  if (length == 0) {
    os::close(fd.get());
    return std::make_tuple(size, "");
  }

  // Cap the read length at 16 pages.
  length = std::min(length.get(), os::pagesize() * 16);

  // Seek to the offset we want to read from.
  lseek = os::lseek(fd.get(), static_cast<off_t>(offset), SEEK_SET);
  if (lseek.isError()) {
    string error = strings::format(
        "Failed to seek file at '%s': %s",
        resolvedPath.get(),
        os::strerror(errno)).get();

    LOG(WARNING) << error;
    os::close(fd.get());
    return FilesError(FilesError::Type::UNKNOWN, error);
  }

  Try<Nothing> async = io::prepare_async(fd.get());
  if (async.isError()) {
    string error =
        "Failed to make file descriptor asynchronous: " + async.error();
    LOG(WARNING) << error;
    os::close(fd.get());
    return FilesError(FilesError::Type::UNKNOWN, error);
  }

  // Read 'length' bytes (or to EOF).
  boost::shared_array<char> data(new char[length.get()]);

  return io::read(fd.get(), data.get(), length.get())
    .then([size, data](const size_t dataLength)
        -> Try<tuple<size_t, string>, FilesError> {
      return std::make_tuple(size, string(data.get(), dataLength));
    })
    .onAny([fd]() { os::close(fd.get()); });
}


const string FilesProcess::DOWNLOAD_HELP = HELP(
    TLDR(
        "Returns the raw file contents for a given path."),
    DESCRIPTION(
        "This endpoint will return the raw file contents for the",
        "given path.",
        "",
        "Query parameters:",
        "",
        ">        path=VALUE          The path of directory to browse."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Downloading files requires that the request principal is",
        "authorized to do so for the target virtual file path.",
        "",
        "Authorizers may categorize different virtual paths into",
        "different ACLs, e.g. logs in one and task sandboxes in",
        "another.",
        "",
        "See authorization documentation for details."));


Future<http::Response> FilesProcess::download(
    const http::Request& request,
    const Option<Principal>& principal)
{
  Option<string> path = request.url.query.get("path");

  if (!path.isSome() || path->empty()) {
    return BadRequest("Expecting 'path=value' in query.\n");
  }

  const string requestedPath = path::from_uri(path.get());

  return authorize(requestedPath, principal)
    .then(defer(self(),
        [this, requestedPath](bool authorized) -> Future<http::Response> {
      if (authorized) {
        return _download(requestedPath);
      }

      return Forbidden();
    }));
}


Future<http::Response> FilesProcess::_download(const string& path)
{
  Result<string> resolvedPath = resolve(path);

  if (resolvedPath.isError()) {
    return BadRequest(resolvedPath.error() + ".\n");
  } else if (!resolvedPath.isSome()) {
    return NotFound();
  }

  // Don't download directories.
  if (os::stat::isdir(resolvedPath.get())) {
    return BadRequest("Cannot download a directory.\n");
  }

  string basename = Path(resolvedPath.get()).basename();

  OK response;
  response.type = response.PATH;
  response.path = resolvedPath.get();
  response.headers["Content-Type"] = "application/octet-stream";
  response.headers["Content-Disposition"] =
    strings::format("attachment; filename=%s", basename).get();

  // Attempt to detect the mime type.
  Option<string> extension = Path(resolvedPath.get()).extension();

  if (extension.isSome() && mime::types.count(extension.get()) > 0) {
    response.headers["Content-Type"] = mime::types[extension.get()];
  }

  return response;
}


const string FilesProcess::DEBUG_HELP = HELP(
    TLDR(
        "Returns the internal virtual path mapping."),
    DESCRIPTION(
        "This endpoint shows the internal virtual path map as a",
        "JSON object."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "The request principal should be authorized to query this endpoint.",
        "See the authorization documentation for details."));


Future<http::Response> FilesProcess::debug(
    const http::Request& request,
    const Option<Principal>& principal)
{
  JSON::Object object;
  foreachpair (const string& virtualPath, const string& path, paths) {
    object.values[virtualPath] = path;
  }

  const Option<string>& jsonp = request.url.query.get("jsonp");

  return authorizeEndpoint(
      request.url.path,
      request.method,
      authorizer,
      principal)
    .then(defer(
        [object, jsonp](bool authorized) -> Future<http::Response> {
          if (!authorized) {
            return Forbidden();
          }

          return OK(object, jsonp);
        }));
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
      strings::remove(path, stringify(os::PATH_SEPARATOR), strings::SUFFIX),
      stringify(os::PATH_SEPARATOR));

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
    if (os::stat::isdir(path)) {
      path = path::join(path, suffix, os::PATH_SEPARATOR);

      // Canonicalize the absolute path.
      Result<string> realpath = os::realpath(path);
      if (realpath.isError()) {
        return Error(
            "Failed to determine canonical path of '" + path +
            "': " + realpath.error());
      } else if (realpath.isNone()) {
        return None();
      }

      // Make sure the canonicalized absolute path is accessible
      // (i.e., not outside the "chroot").
      if (!strings::startsWith(realpath.get(), paths[prefix])) {
        return Error("'" + path + "' is inaccessible");
      }

      path = realpath.get();
    } else if (suffix != "") {
      // Request is assuming attached path is a directory, but it is
      // not! Rather than 'Bad Request', treat this as 'Not Found'.
      return None();
    }

    return path;
  }

  return None();
}


Files::Files(const Option<string>& authenticationRealm,
             const Option<Authorizer*>& authorizer)
{
  process = new FilesProcess(authenticationRealm, authorizer);
  spawn(process);
}


Files::~Files()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Nothing> Files::attach(
    const string& path,
    const string& virtualPath,
    const Option<lambda::function<Future<bool>(const Option<Principal>&)>>&
        authorized)
{
  return dispatch(
      process,
      &FilesProcess::attach,
      path,
      virtualPath,
      authorized);
}


void Files::detach(const string& virtualPath)
{
  dispatch(process, &FilesProcess::detach, virtualPath);
}


Future<Try<list<FileInfo>, FilesError>> Files::browse(
    const string& path,
    const Option<Principal>& principal)
{
  return dispatch(process, &FilesProcess::browse, path, principal);
}


Future<Try<tuple<size_t, string>, FilesError>> Files::read(
    const size_t offset,
    const Option<size_t>& length,
    const string& path,
    const Option<Principal>& principal)
{
  return dispatch(process,
                  &FilesProcess::read,
                  offset,
                  length,
                  path,
                  principal);
}

} // namespace internal {
} // namespace mesos {
