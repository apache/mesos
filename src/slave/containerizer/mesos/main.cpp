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

#include <stout/none.hpp>
#include <stout/subcommand.hpp>

#include <stout/os/socket.hpp>

#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/mount.hpp"

#ifdef __linux__
#include "slave/containerizer/mesos/isolators/network/cni/cni.hpp"
#endif

using namespace mesos::internal::slave;


int main(int argc, char** argv)
{
#ifdef __WINDOWS__
  // Initialize the Windows socket stack.
  if (!net::wsa_initialize()) {
    EXIT(EXIT_FAILURE) << "Failed to initialize the WSA socket stack";
  }
#endif // __WINDOWS__

#ifdef __linux__
  int success = Subcommand::dispatch(
      "MESOS_CONTAINERIZER_",
      argc,
      argv,
      new MesosContainerizerLaunch(),
      new MesosContainerizerMount(),
      new NetworkCniIsolatorSetup());
#else
  int success = Subcommand::dispatch(
      "MESOS_CONTAINERIZER_",
      argc,
      argv,
      new MesosContainerizerLaunch(),
      new MesosContainerizerMount());
#endif

#ifdef __WINDOWS__
  if (!net::wsa_cleanup()) {
    EXIT(EXIT_FAILURE) << "Failed to cleanup the WSA socket stack";
  }
#endif // __WINDOWS__

  return success;
}
