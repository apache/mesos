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

#ifndef __LOCAL_FLAGS_HPP__
#define __LOCAL_FLAGS_HPP__

#include <stout/flags.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "logging/flags.hpp"

namespace mesos {
namespace internal {
namespace local {

class Flags : public virtual logging::Flags
{
public:
  Flags()
  {
    add(&Flags::work_dir,
        "work_dir",
        "Path of the master/agent work directory. This is where the\n"
        "persistent information of the cluster will be stored.\n\n"
        "NOTE: Locations like `/tmp` which are cleaned automatically\n"
        "are not suitable for the work directory when running in\n"
        "production, since long-running masters and agents could lose\n"
        "data when cleanup occurs. Local mode is used explicitly for\n"
        "non-production purposes, so this is the only case where having\n"
        "a default `work_dir` flag is acceptable.\n"
        "(Example: `/var/lib/mesos`)\n\n"
        "Individual work directories for each master and agent will be\n"
        "nested underneath the given work directory:\n"
        "root (`work_dir` flag)\n"
        "|-- agents\n"
        "|   |-- 0\n"
        "|   |   |-- fetch (--fetcher_cache_dir)\n"
        "|   |   |-- run   (--runtime_dir)\n"
        "|   |   |-- work  (--work_dir)\n"
        "|   |-- 1\n"
        "|   |   ...\n"
        "|-- master",
        path::join(os::temp(), "mesos", "work"));

    add(&Flags::num_slaves,
        "num_slaves",
        "Number of agents to launch for local cluster",
        1);
  }

  std::string work_dir;
  int num_slaves;
};

} // namespace local {
} // namespace internal {
} // namespace mesos {

#endif // __LOCAL_FLAGS_HPP__
