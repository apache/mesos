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

#ifndef __WINDOWS_ISOLATOR_HPP__
#define __WINDOWS_ISOLATOR_HPP__

#include <process/future.hpp>
#include <process/id.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"
#include "slave/containerizer/mesos/isolators/posix.hpp"

namespace mesos {
namespace internal {
namespace slave {

// A basic MesosIsolatorProcess that keeps track of the pid but
// doesn't do any resource isolation. Subclasses must implement
// usage() for their appropriate resource(s).
//
// TODO(hausdorff): (MESOS-5462) For now the Windows isolators are essentially
// direct copies of their POSIX counterparts. In the future, we expect to
// refactor the POSIX classes into platform-independent base class, with
// Windows and POSIX implementations. For now, we leave the Windows
// implementations as inheriting from the POSIX implementations.
class WindowsIsolatorProcess : public PosixIsolatorProcess {};


// TODO(hausdorff): (MESOS-5462) For now the Windows isolators are essentially
// direct copies of their POSIX counterparts. In the future, we expect to
// refactor the POSIX classes into platform-independent base class, with
// Windows and POSIX implementations. For now, we leave the Windows
// implementations as inheriting from the POSIX implementations.
class WindowsCpuIsolatorProcess : public PosixCpuIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags)
  {
    process::Owned<MesosIsolatorProcess> process(
        new WindowsCpuIsolatorProcess());

    return new MesosIsolator(process);
  }

private:
  WindowsCpuIsolatorProcess()
    : ProcessBase(process::ID::generate("windows-cpu-isolator")) {}
};


// TODO(hausdorff): (MESOS-5462) For now the Windows isolators are essentially
// direct copies of their POSIX counterparts. In the future, we expect to
// refactor the POSIX classes into platform-independent base class, with
// Windows and POSIX implementations. For now, we leave the Windows
// implementations as inheriting from the POSIX implementations.
class WindowsMemIsolatorProcess : public PosixMemIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags)
  {
    process::Owned<MesosIsolatorProcess> process(
        new WindowsMemIsolatorProcess());

    return new MesosIsolator(process);
  }

private:
  WindowsMemIsolatorProcess()
    : ProcessBase(process::ID::generate("windows-mem-isolator")) {}
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __WINDOWS_ISOLATOR_HPP__
