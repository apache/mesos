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
    // `work_dir` is passed from here to the agents/master.
    // This is necessary because `work_dir` is a required flag
    // in agents/master and without this, the load call for their
    // flags will spit out an error unless they have an env
    // variable for the `work_dir` explicitly set.
    // Since local mode is used strictly for non-production
    // purposes, it is the one case where we deem it acceptable
    // to set a default value for `work_dir`.
    add(&Flags::work_dir,
        "work_dir",
        "Path of the master/agent work directory. This is where the\n"
        "persistent information of the cluster will be stored.\n"
        "Note that locations like `/tmp` which are cleaned\n"
        "automatically are not suitable for the work directory\n"
        "when running in production, since long-running masters\n"
        "and agents could lose data when cleanup occurs.\n"
        "(Example: `/var/lib/mesos`)",
        path::join(os::temp(), "mesos", "work"));

    add(&Flags::runtime_dir,
        "runtime_dir",
        "Path of the agent runtime directory. This is where runtime\n"
        "data is stored by an agent that it needs to persist across\n"
        "crashes (but not across reboots). This directory will be\n"
        "cleared on reboot.\n"
        "(Example: `/var/run/mesos`)",
        path::join(os::temp(), "mesos", "runtime"));

    add(&Flags::num_slaves,
        "num_slaves",
        "Number of agents to launch for local cluster",
        1);
  }

  std::string work_dir;
  std::string runtime_dir;
  int num_slaves;
};

} // namespace local {
} // namespace internal {
} // namespace mesos {

#endif // __LOCAL_FLAGS_HPP__
