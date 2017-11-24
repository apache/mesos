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

#include <glog/logging.h>

#include <mesos/type_utils.hpp>

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "resource_provider/local.hpp"

namespace http = process::http;

using std::list;
using std::string;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::ProcessBase;

using process::dispatch;
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
      const http::URL& _url,
      const string& _workDir,
      const Option<string>& _configDir)
    : ProcessBase(process::ID::generate("local-resource-provider-daemon")),
      url(_url),
      workDir(_workDir),
      configDir(_configDir) {}

  LocalResourceProviderDaemonProcess(
      const LocalResourceProviderDaemonProcess& other) = delete;

  LocalResourceProviderDaemonProcess& operator=(
      const LocalResourceProviderDaemonProcess& other) = delete;

  void start(const SlaveID& _slaveId);

protected:
  void initialize() override;

private:
  struct ProviderData
  {
    ProviderData(const ResourceProviderInfo& _info)
      : info(_info) {}

    const ResourceProviderInfo info;
    Owned<LocalResourceProvider> provider;
  };

  Try<Nothing> load(const string& path);

  Future<Nothing> launch(const string& type, const string& name);

  const http::URL url;
  const string workDir;
  const Option<string> configDir;

  Option<SlaveID> slaveId;
  hashmap<string, hashmap<string, ProviderData>> providers;
};


void LocalResourceProviderDaemonProcess::start(const SlaveID& _slaveId)
{
  // NOTE: It's possible that the slave receives multiple
  // `SlaveRegisteredMessage`s and detects a disconnection in between.
  // In that case, `start` will be called multiple times from
  // `Slave::registered`.
  if (slaveId.isSome()) {
    CHECK_EQ(slaveId.get(), _slaveId)
      << "Cannot start local resource provider daemon with id " << _slaveId
      << " (expected: " << slaveId.get() << ")";

    return;
  }

  slaveId = _slaveId;

  foreachkey (const string& type, providers) {
    foreachkey (const string& name, providers[type]) {
      auto error = [=](const string& message) {
        LOG(ERROR) << "Failed to launch resource provider with type '" << type
                   << "' and name '" << name << "': " << message;
      };

      launch(type, name)
        .onFailed(error)
        .onDiscarded(std::bind(error, "future discarded"));
    }
  }
}


void LocalResourceProviderDaemonProcess::initialize()
{
  if (configDir.isNone()) {
    return;
  }

  Try<list<string>> entries = os::ls(configDir.get());
  if (entries.isError()) {
    LOG(FATAL) << "Unable to list the resource provider config directory '"
               << configDir.get() << "': " << entries.error();
    return;
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
  if (providers[info->type()].contains(info->name())) {
    return Error(
        "Multiple resource providers with type '" + info->type() +
        "' and name '" + info->name() + "'");
  }

  providers[info->type()].put(info->name(), info.get());

  return Nothing();
}


Future<Nothing> LocalResourceProviderDaemonProcess::launch(
    const string& type,
    const string& name)
{
  CHECK_SOME(slaveId);
  CHECK(providers[type].contains(name));

  ProviderData& data = providers[type].at(name);

  Try<Owned<LocalResourceProvider>> provider =
    LocalResourceProvider::create(url, data.info);

  if (provider.isError()) {
    return Failure(
        "Failed to create resource provider with type '" + type +
        "' and name '" + name + "': " + provider.error());
  }

  data.provider = provider.get();

  return Nothing();
}


Try<Owned<LocalResourceProviderDaemon>> LocalResourceProviderDaemon::create(
    const http::URL& url,
    const slave::Flags& flags)
{
  // We require that the config directory exists to create a daemon.
  Option<string> configDir = flags.resource_provider_config_dir;
  if (configDir.isSome() && !os::exists(configDir.get())) {
    return Error("Config directory '" + configDir.get() + "' does not exist");
  }

  return new LocalResourceProviderDaemon(url, flags.work_dir, configDir);
}


LocalResourceProviderDaemon::LocalResourceProviderDaemon(
    const http::URL& url,
    const string& workDir,
    const Option<string>& configDir)
  : process(new LocalResourceProviderDaemonProcess(url, workDir, configDir))
{
  spawn(CHECK_NOTNULL(process.get()));
}


LocalResourceProviderDaemon::~LocalResourceProviderDaemon()
{
  terminate(process.get());
  wait(process.get());
}


void LocalResourceProviderDaemon::start(const SlaveID& slaveId)
{
  dispatch(process.get(), &LocalResourceProviderDaemonProcess::start, slaveId);
}

} // namespace internal {
} // namespace mesos {
