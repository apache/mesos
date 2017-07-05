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

#include <utility>
#include <vector>

#include <glog/logging.h>

#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "resource_provider/local.hpp"

using std::list;
using std::string;
using std::vector;

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
  struct Provider
  {
    Provider(const ResourceProviderInfo& _info,
             Owned<LocalResourceProvider> _provider)
      : info(_info),
        provider(std::move(_provider)) {}

    const ResourceProviderInfo info;
    const Owned<LocalResourceProvider> provider;
  };

  Try<Nothing> load(const string& path);

  const string workDir;
  const Option<string> configDir;

  vector<Provider> providers;
};


void LocalResourceProviderDaemonProcess::initialize()
{
  if (configDir.isNone()) {
    return;
  }

  Try<list<string>> entries = os::ls(configDir.get());
  if (entries.isError()) {
    LOG(ERROR) << "Unable to list the resource provider directory '"
               << configDir.get() << "': " << entries.error();
  }

  foreach (const string& entry, entries.get()) {
    const string path = path::join(configDir.get(), entry);

    if (os::stat::isdir(path)) {
      continue;
    }

    Try<Nothing> loading = load(path);
    if (loading.isError()) {
      LOG(ERROR) << "Failed to load resource provider config '"
                 << path << "': " << loading.error();
      continue;
    }
  }
}


Try<Nothing> LocalResourceProviderDaemonProcess::load(const string& path)
{
  Try<string> read = os::read(path);
  if (read.isError()) {
    return Error("Failed to read the config file: " + read.error());
  }

  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  if (json.isError()) {
    return Error("Failed to parse the JSON config: " + json.error());
  }

  Try<ResourceProviderInfo> info =
    ::protobuf::parse<ResourceProviderInfo>(json.get());

  if (info.isError()) {
    return Error("Not a valid resource provider config: " + info.error());
  }

  // Ensure that ('type', 'name') pair is unique.
  foreach (const Provider& provider, providers) {
    if (info->type() == provider.info.type() &&
        info->name() == provider.info.name()) {
      return Error(
          "Multiple resource providers with type '" + info->type() +
          "' and name '" + info->name() + "'");
    }
  }

  Try<Owned<LocalResourceProvider>> provider =
    LocalResourceProvider::create(info.get());

  if (provider.isError()) {
    return Error(
        "Failed to create resource provider with type '" + info->type() +
        "' and name '" + info->name() + "'");
  }

  providers.emplace_back(info.get(), provider.get());

  return Nothing();
}


Try<Owned<LocalResourceProviderDaemon>> LocalResourceProviderDaemon::create(
    const slave::Flags& flags)
{
  // We require that the config directory exists to create a daemon.
  Option<string> configDir = flags.resource_provider_config_dir;
  if (configDir.isSome() && !os::exists(configDir.get())) {
    return Error("Config directory '" + configDir.get() + "' does not exist");
  }

  return new LocalResourceProviderDaemon(
      flags.work_dir,
      configDir);
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
