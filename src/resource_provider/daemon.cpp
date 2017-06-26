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

#include "resource_provider/daemon.hpp"

#include <process/id.hpp>
#include <process/process.hpp>

#include "resource_provider/local.hpp"

using std::string;

using process::Owned;
using process::Process;
using process::ProcessBase;

using process::spawn;
using process::terminate;
using process::wait;

namespace mesos {
namespace internal {

class LocalResourceProviderDaemonProcess
  : public Process<LocalResourceProviderDaemonProcess>
{
public:
  LocalResourceProviderDaemonProcess(
      const string& _workDir,
      const Option<string>& _configDir)
    : ProcessBase(process::ID::generate("local-resource-provider-daemon")),
      workDir(_workDir),
      configDir(_configDir) {}

protected:
  void initialize() override;

private:
  const string workDir;
  const Option<string> configDir;
};


void LocalResourceProviderDaemonProcess::initialize()
{
}


Try<Owned<LocalResourceProviderDaemon>> LocalResourceProviderDaemon::create(
    const slave::Flags& flags)
{
  return new LocalResourceProviderDaemon(
      flags.work_dir,
      flags.resource_provider_config_dir);
}


LocalResourceProviderDaemon::LocalResourceProviderDaemon(
    const string& workDir,
    const Option<string>& configDir)
  : process(new LocalResourceProviderDaemonProcess(workDir, configDir))
{
  spawn(CHECK_NOTNULL(process.get()));
}


LocalResourceProviderDaemon::~LocalResourceProviderDaemon()
{
  terminate(process.get());
  wait(process.get());
}

} // namespace internal {
} // namespace mesos {
