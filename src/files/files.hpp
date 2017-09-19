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

#ifndef __FILES_HPP__
#define __FILES_HPP__

#include <string>

#include <mesos/authorizer/authorizer.hpp>

#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/format.hpp>
#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>

#include "common/protobuf_utils.hpp"

#include "mesos/mesos.hpp"

namespace mesos {
namespace internal {

// Forward declarations.
class FilesProcess;


// Represents the various errors that can be returned by methods on the `Files`
// class via a `Try` that has failed.
class FilesError : public Error
{
public:
  enum Type
  {
    INVALID,        // Invalid argument.
    NOT_FOUND,      // Not found.
    UNAUTHORIZED,   // Not authorized to perform the operation.
    UNKNOWN         // Internal error/all other errors.
  };

  FilesError(Type _type)
    : Error(stringify(_type)), type(_type) {}

  FilesError(Type _type, const std::string& _message)
    : Error(stringify(_type)), type(_type), message(_message) {}

  Type type;
  std::string message;
};


// Provides an abstraction for browsing and reading files via HTTP
// endpoints. A path (file or directory) may be "attached" to a virtual path
// (similar to "mounting" a device) for subsequent browsing and
// reading of any files and directories it contains. The "mounting" of
// paths to virtual paths enables us to do a form of chrooting for better
// security and isolation of files.
class Files
{
public:
  Files(const Option<std::string>& authenticationRealm = None(),
        const Option<mesos::Authorizer*>& authorizer = None());
  ~Files();

  // Returns the result of trying to attach the specified path
  // (directory or file) at the specified virtual path.
  process::Future<Nothing> attach(
      const std::string& path,
      const std::string& virtualPath,
      const Option<lambda::function<process::Future<bool>(
          const Option<process::http::authentication::Principal>&)>>&
              authorized = None());

  // Removes the specified virtual path.
  void detach(const std::string& virtualPath);

  // Returns a file listing for a directory similar to `ls -l`.
  process::Future<Try<std::list<FileInfo>, FilesError>> browse(
      const std::string& path,
      const Option<process::http::authentication::Principal>& principal);

  // Returns the size and data of file.
  process::Future<Try<std::tuple<size_t, std::string>, FilesError>> read(
      const size_t offset,
      const Option<size_t>& length,
      const std::string& path,
      const Option<process::http::authentication::Principal>& principal);

private:
  FilesProcess* process;
};

} // namespace internal {
} // namespace mesos {

#endif // __FILES_HPP__
