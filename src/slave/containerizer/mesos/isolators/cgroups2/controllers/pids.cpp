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

#include <process/id.hpp>

#include "slave/containerizer/mesos/isolators/cgroups2/controllers/pids.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/constants.hpp"

using process::Owned;

using std::string;

namespace mesos {
namespace internal {
namespace slave {

Try<Owned<ControllerProcess>> PidsControllerProcess::create(const Flags& flags)
{
  return Owned<ControllerProcess>(new PidsControllerProcess(flags));
}


PidsControllerProcess::PidsControllerProcess(const Flags& _flags)
  : ProcessBase(process::ID::generate("cgroups-v2-pids-controller")),
    ControllerProcess(_flags) {}


string PidsControllerProcess::name() const
{
  return CGROUPS2_CONTROLLER_PIDS_NAME;
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {