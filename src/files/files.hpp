/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __FILES_HPP__
#define __FILES_HPP__

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>

namespace mesos {
namespace internal {

// Forward declarations.
class FilesProcess;


// Provides an abstraction for browsing and reading files via HTTP
// endpoints. A path (file or directory) may be "attached" to a name
// (similar to "mounting" a device) for subsequent browsing and
// reading of any files and directories it contains. The "mounting" of
// paths to names enables us to do a form of chrooting for better
// security and isolation of files.
class Files
{
public:
  Files();
  ~Files();

  // Returns the result of trying to attach the specified path
  // (directory or file) at the specified name.
  process::Future<Nothing> attach(
      const std::string& path,
      const std::string& name);

  // Removes the specified name.
  void detach(const std::string& name);

  // Returns the pid for the FilesProcess.
  // NOTE: This has been made visible for testing.
  process::PID<> pid();

private:
  FilesProcess* process;
};


// Returns our JSON representation of a file or directory.
inline JSON::Object jsonFileInfo(const std::string& path,
                                 bool isDir,
                                 size_t size)
{
  JSON::Object file;
  file.values["path"] = path;
  file.values["size"] = size;

  if (isDir) {
    file.values["dir"] = JSON::True();
  } else {
    file.values["dir"] = JSON::False();
  }

  return file;
}

} // namespace internal {
} // namespace mesos {

#endif // __FILES_HPP__
