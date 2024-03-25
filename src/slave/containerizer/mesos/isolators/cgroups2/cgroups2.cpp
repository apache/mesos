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

#include "slave/containerizer/mesos/isolators/cgroups2/cgroups2.hpp"

#include <string>

#include <process/id.hpp>

using mesos::slave::Isolator;

using process::Owned;

using std::string;

namespace mesos {
namespace internal {
namespace slave {

Cgroups2IsolatorProcess::Cgroups2IsolatorProcess(
  const hashmap<string, Owned<Subsystem>>& _subsystems)
  : ProcessBase(process::ID::generate("cgroups2-isolator")),
    subsystems(_subsystems) {}


Cgroups2IsolatorProcess::~Cgroups2IsolatorProcess() {}


Try<Isolator*> Cgroups2IsolatorProcess::create(const Flags& flags)
{
  hashmap<string, Owned<Subsystem>> subsystems;

  Owned<MesosIsolatorProcess> process(new Cgroups2IsolatorProcess(subsystems));
  return new MesosIsolator(process);
}


bool Cgroups2IsolatorProcess::supportsNesting()
{
  // TODO(dleamy): Update this once cgroups v2 supports nested containers.
  return false;
}


bool Cgroups2IsolatorProcess::supportsStandalone()
{
  return true;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
