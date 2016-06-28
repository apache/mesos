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

#ifndef __LAUNCHER_POSIX_EXECUTOR_HPP__
#define __LAUNCHER_POSIX_EXECUTOR_HPP__

#include <string>

#include <mesos/v1/mesos.hpp>

#include <stout/option.hpp>

namespace mesos {
namespace v1 {
namespace internal {

pid_t launchTaskPosix(
    const mesos::v1::CommandInfo& command,
    const Option<std::string>& user,
    char** argv,
    Option<std::string>& rootfs,
    Option<std::string>& sandboxDirectory,
    Option<std::string>& workingDirectory);

} // namespace internal {
} // namespace v1 {
} // namespace mesos {

#endif // __LAUNCHER_POSIX_EXECUTOR_HPP__
