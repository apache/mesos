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

#ifndef __SLAVE_FETCHER_HPP__
#define __SLAVE_FETCHER_HPP__

#include <map>
#include <string>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <mesos/mesos.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace fetcher {

// Defines helpers for running the mesos-fetcher.
// TODO(benh): Consider moving this into a 'fetcher' subdirectory as
// well as the actual mesos-fetcher program (in launcher/fetcher.cpp
// as of the writing of this comment).

// Helper method to build the environment used to run mesos-fetcher.
std::map<std::string, std::string> environment(
    const CommandInfo& commandInfo,
    const std::string& directory,
    const Option<std::string>& user,
    const Flags& flags);

// Run the mesos-fetcher for the specified arguments. Note that if
// 'stdout' and 'stderr' file descriptors are provided then respective
// output from the mesos-fetcher will be redirected to the file
// descriptors. The file descriptors are duplicated (via dup) because
// redirecting might still be occuring even after the mesos-fetcher has
// terminated since there still might be data to be read.
Try<process::Subprocess> run(
    const CommandInfo& commandInfo,
    const std::string& directory,
    const Option<std::string>& user,
    const Flags& flags,
    const Option<int>& stdout,
    const Option<int>& stderr);

// Run the mesos-fetcher for the specified arguments, creating a
// "stdout" and "stderr" file in the given directory and using
// these for output.
Try<process::Subprocess> run(
    const CommandInfo& commandInfo,
    const std::string& directory,
    const Option<std::string>& user,
    const Flags& flags);

// Check status and return an error if any. Typically used after
// calling run().
process::Future<Nothing> _run(
    const ContainerID& containerId,
    const Option<int>& status);

} // namespace fetcher {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_FETCHER_HPP__
