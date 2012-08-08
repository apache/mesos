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
  process::Future<bool> attach(
      const std::string& path,
      const std::string& name);

  // Removes the specified name.
  void detach(const std::string& name);

private:
  FilesProcess* process;
};

} // namespace internal {
} // namespace mesos {

#endif // __FILES_HPP__
