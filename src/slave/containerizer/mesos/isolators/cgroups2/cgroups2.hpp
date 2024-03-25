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

#ifndef __CGROUPS_V2_ISOLATOR_HPP__
#define __CGROUPS_V2_ISOLATOR_HPP__

#include <string>

#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/mesos/isolator.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystem.hpp"
#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

class Cgroups2IsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  ~Cgroups2IsolatorProcess() override;

  bool supportsNesting() override;

  bool supportsStandalone() override;

private:
  Cgroups2IsolatorProcess(
      const hashmap<std::string, process::Owned<Subsystem>>& _subsystems);

  // Maps each subsystems to the `Subsystem` isolator that manages it.
  const hashmap<std::string, process::Owned<Subsystem>> subsystems;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif
