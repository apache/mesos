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

#ifndef __TEST_ROOTFS_HPP__
#define __TEST_ROOTFS_HPP__

#include <string>

#include <process/owned.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace tests {

class Rootfs {
public:
  virtual ~Rootfs();

  // Add a host directory or file to the root filesystem. Note that
  // the host directory or file needs to be an absolute path.
  Try<Nothing> add(const std::string& path);

  const std::string root;

protected:
  Rootfs(const std::string& _root) : root(_root) {}
};


class LinuxRootfs : public Rootfs
{
public:
  static Try<process::Owned<Rootfs>> create(const std::string& root);

protected:
  LinuxRootfs(const std::string& root) : Rootfs(root) {}
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_ROOTFS_HPP__
