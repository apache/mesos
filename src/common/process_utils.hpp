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

#ifndef __PROCESS_UTILS_HPP__
#define __PROCESS_UTILS_HPP__

#include <iostream>
#include <sstream>

#include "common/utils.hpp"


namespace mesos {
namespace internal {
namespace utils {
namespace process {

inline Try<int> killtree(
    pid_t pid,
    int signal,
    bool killgroups,
    bool killsess)
{
  if (utils::os::hasenv("MESOS_HOME")) {
    return Try<int>::error("Expecting MESOS_HOME to be set");
  }

  std::string cmdline = utils::os::getenv("MESOS_HOME");
  cmdline += "/killtree.sh";
  cmdline += " -p " + pid;
  cmdline += " -s " + signal;
  if (killgroups) cmdline += " -g";
  if (killsess) cmdline += " -x";

  return utils::os::shell(NULL, cmdline);
}

} // namespace mesos {
} // namespace internal {
} // namespace utils {
} // namespace process {

#endif // __PROCESS_UTILS_HPP__
