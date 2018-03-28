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

#include <process/defer.hpp>
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

#include "common/validation.hpp"

#include "resource_provider/local.hpp"

namespace http = process::http;

using std::list;
using std::string;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::ProcessBase;

using process::defer;
using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

using process::http::authentication::Principal;

namespace mesos {
namespace internal {

// Directory under the resource provider config directory to store
// temporarily files for adding and updating resource provider config
// files atomically (all-or-nothing).
constexpr char STAGING_DIR[] = ".staging";


class LocalResourceProviderDaemonProcess
  : public Process<LocalResourceProviderDaemonProcess>
{
public:
  LocalResourceProviderDaemonProcess(
      const http::URL& _url,
      const string& _workDir,
      const Option<string>& _configDir,
      SecretGenerator* _secretGenerator,
      bool _strict)
    : ProcessBase(process::ID::generate("local-resource-provider-daemon")),
      url(_url),
      workDir(_workDir),
      configDir(_configDir),
      secretGenerator(_secretGenerator),
      strict(_strict) {}

  LocalResourceProviderDaemonProcess(
      const LocalResourceProviderDaemonProcess& other) = delete;

  LocalResourceProviderDaemonProcess& operator=(
      const LocalResourceProviderDaemonProcess& other) = delete;

  void start(const SlaveID& _slaveId);

  Future<bool> add(const ResourceProviderInfo& info);
  Future<bool> update(const ResourceProviderInfo& info);
  Future<Nothing> remove(const string& type, const string& name);

protected:
  void initialize() override;

private:
  struct ProviderData
  {
    ProviderData(const string& _path, const ResourceProviderInfo& _info)
      : path(_path), info(_info), version(id::UUID::random()) {}

    const string path;
    ResourceProviderInfo info;

    // The `version` is used to check if `provider` holds a resource
    // provider instance that is in sync with the current config.
    id::UUID version;
    Owned<LocalResourceProvider> provider;
  };

  Try<Nothing> load(const string& path);
  Try<Nothing> save(const string& path, const ResourceProviderInfo& info);

  // NOTE: `launch` should only be called once for each config version.
  // It will pick up the latest config to launch the resource provider.
  Future<Nothing> launch(
      const string& type,
      const string& name);

  Future<Nothing> _launch(
      const string& type,
      const string& name,
      const id::UUID& version,
      const Option<string>& authToken);

  Future<Option<string>> generateAuthToken(const ResourceProviderInfo& info);

  const http::URL url;
  const string workDir;
  const Option<string> configDir;
  SecretGenerator* const secretGenerator;
  const bool strict;

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


Future<bool> LocalResourceProviderDaemonProcess::add(
    const ResourceProviderInfo& info)
{
  CHECK(!info.has_id()); // Should have already been validated by the agent.

  if (configDir.isNone()) {
    return Failure("Missing required flag --resource_provider_config_dir");
  }

  // Return true if the info has been added for idempotency.
  if (providers[info.type()].contains(info.name())) {
    return providers[info.type()].at(info.name()).info == info;
  }

  // Generate a filename for the config.
  // NOTE: We use the following template `<type>.<name>.<uuid>.json`
  // with a random UUID to generate a new filename to avoid any conflict
  // with existing ad-hoc config files.
  const string path = path::join(
      configDir.get(),
      strings::join(".", info.type(), info.name(), id::UUID::random(), "json"));

  LOG(INFO) << "Creating new config file '" << path << "'";

  Try<Nothing> _save = save(path, info);
  if (_save.isError()) {
    return Failure(
        "Failed to write config file '" + path + "': " + _save.error());
  }

  providers[info.type()].put(info.name(), {path, info});

  // Launch the resource provider if the daemon is already started.
  if (slaveId.isSome()) {
    auto err = [](const ResourceProviderInfo& info, const string& message) {
      LOG(ERROR)
        << "Failed to launch resource provider with type '" << info.type()
        << "' and name '" << info.name() << "': " << message;
    };

    launch(info.type(), info.name())
      .onFailed(std::bind(err, info, lambda::_1))
      .onDiscarded(std::bind(err, info, "future discarded"));
  }

  return true;
}


Future<bool> LocalResourceProviderDaemonProcess::update(
    const ResourceProviderInfo& info)
{
  CHECK(!info.has_id()); // Should have already been validated by the agent.

  if (configDir.isNone()) {
    return Failure("Missing required flag --resource_provider_config_dir");
  }

  if (!providers[info.type()].contains(info.name())) {
    return false;
  }

  ProviderData& data = providers[info.type()].at(info.name());

  // Return true if the info has been updated for idempotency.
  if (data.info == info) {
    return true;
  }

  Try<Nothing> _save = save(data.path, info);
  if (_save.isError()) {
    return Failure(
        "Failed to write config file '" + data.path + "': " + _save.error());
  }

  data.info = info;

  // Update `version` to indicate that the config has been updated.
  data.version = id::UUID::random();

  // Launch the resource provider if the daemon is already started.
  if (slaveId.isSome()) {
    auto err = [](const ResourceProviderInfo& info, const string& message) {
      LOG(ERROR)
        << "Failed to launch resource provider with type '" << info.type()
        << "' and name '" << info.name() << "': " << message;
    };

    launch(info.type(), info.name())
      .onFailed(std::bind(err, info, lambda::_1))
      .onDiscarded(std::bind(err, info, "future discarded"));
  }

  return true;
}


Future<Nothing> LocalResourceProviderDaemonProcess::remove(
    const string& type,
    const string& name)
{
  if (configDir.isNone()) {
    return Failure("Missing required flag --resource_provider_config_dir");
  }

  // Do nothing if the info has been removed for idempotency.
  if (!providers[type].contains(name)) {
    return Nothing();
  }

  const string path = providers[type].at(name).path;

  Try<Nothing> rm = os::rm(path);
  if (rm.isError()) {
    return Failure(
        "Failed to remove config file '" + path + "': " + rm.error());
  }

  // Removing the provider data from `providers` will cause the resource
  // provider to be destructed.
  providers[type].erase(name);

  return Nothing();
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

    // Skip subdirectories in the resource provider config directory,
    // including the staging directory.
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

  if (info->has_id()) {
    return Error("'ResourceProviderInfo.id' must not be set");
  }

  // Ensure that ('type', 'name') pair is unique.
  if (providers[info->type()].contains(info->name())) {
    return Error(
        "Multiple resource providers with type '" + info->type() +
        "' and name '" + info->name() + "'");
  }

  providers[info->type()].put(info->name(), {path, std::move(info.get())});

  return Nothing();
}


// NOTE: We provide atomic (all-or-nothing) semantics here by always
// writing to a temporary file to the staging directory first then using
// `os::rename` to atomically move it to the desired path.
Try<Nothing> LocalResourceProviderDaemonProcess::save(
    const string& path,
    const ResourceProviderInfo& info)
{
  CHECK_SOME(configDir);

  // NOTE: We create the staging directory in the resource provider
  // config directory to make sure that the renaming below does not
  // cross devices (MESOS-2319).
  // TODO(chhsiao): Consider adding a way to garbage collect the staging
  // directory.
  const string stagingDir = path::join(configDir.get(), STAGING_DIR);
  Try<Nothing> mkdir = os::mkdir(stagingDir);
  if (mkdir.isError()) {
    return Error(
        "Failed to create directory '" + stagingDir + "': " + mkdir.error());
  }

  const string stagingPath = path::join(stagingDir, Path(path).basename());

  Try<Nothing> write = os::write(stagingPath, stringify(JSON::protobuf(info)));
  if (write.isError()) {
    // Try to remove the temporary file on error.
    os::rm(stagingPath);

    return Error(
        "Failed to write temporary file '" + stagingPath + "': " +
        write.error());
  }

  Try<Nothing> rename = os::rename(stagingPath, path);
  if (rename.isError()) {
    // Try to remove the temporary file on error.
    os::rm(stagingPath);

    return Error(
        "Failed to rename '" + stagingPath + "' to '" + path + "': " +
        rename.error());
  }

  return Nothing();
}


Future<Nothing> LocalResourceProviderDaemonProcess::launch(
    const string& type,
    const string& name)
{
  CHECK_SOME(slaveId);

  // If the resource provider config is removed, nothing needs to be done.
  if (!providers[type].contains(name)) {
    return Nothing();
  }

  ProviderData& data = providers[type].at(name);

  // Destruct the previous resource provider (which will synchronously
  // terminate its actor and driver) if there is one.
  data.provider.reset();

  return generateAuthToken(data.info)
    .then(defer(self(), &Self::_launch, type, name, data.version, lambda::_1));
}


Future<Nothing> LocalResourceProviderDaemonProcess::_launch(
    const string& type,
    const string& name,
    const id::UUID& version,
    const Option<string>& authToken)
{
  // If the resource provider config is removed, abort the launch sequence.
  if (!providers[type].contains(name)) {
    return Nothing();
  }

  ProviderData& data = providers[type].at(name);

  // If there is a version mismatch, abort the launch sequence since
  // `authToken` might be outdated. The callback updating the version
  // should have dispatched another launch sequence.
  if (version != data.version) {
    return Nothing();
  }

  Try<Owned<LocalResourceProvider>> provider = LocalResourceProvider::create(
      url, workDir, data.info, slaveId.get(), authToken, strict);

  if (provider.isError()) {
    return Failure(
        "Failed to create resource provider with type '" + type +
        "' and name '" + name + "': " + provider.error());
  }

  data.provider = provider.get();

  return Nothing();
}


// Generates a secret for local resource provider authentication if needed.
Future<Option<string>> LocalResourceProviderDaemonProcess::generateAuthToken(
    const ResourceProviderInfo& info)
{
  if (secretGenerator == nullptr) {
    return None();
  }

  Try<Principal> principal = LocalResourceProvider::principal(info);

  if (principal.isError()) {
    return Failure(
        "Failed to generate resource provider principal with type '" +
        info.type() + "' and name '" + info.name() + "': " +
        principal.error());
  }

  return secretGenerator->generate(principal.get())
    .then(defer(self(), [](const Secret& secret) -> Future<Option<string>> {
      Option<Error> error = common::validation::validateSecret(secret);

      if (error.isSome()) {
        return Failure(
            "Failed to validate generated secret: " + error->message);
      } else if (secret.type() != Secret::VALUE) {
        return Failure(
            "Expecting generated secret to be of VALUE type instead of " +
            stringify(secret.type()) + " type; " +
            "only VALUE type secrets are supported at this time");
      }

      CHECK(secret.has_value());

      return secret.value().data();
    }));
}


Try<Owned<LocalResourceProviderDaemon>> LocalResourceProviderDaemon::create(
    const http::URL& url,
    const slave::Flags& flags,
    SecretGenerator* secretGenerator)
{
  // We require that the config directory exists to create a daemon.
  Option<string> configDir = flags.resource_provider_config_dir;
  if (configDir.isSome() && !os::exists(configDir.get())) {
    return Error("Config directory '" + configDir.get() + "' does not exist");
  }

  return new LocalResourceProviderDaemon(
      url,
      flags.work_dir,
      configDir,
      secretGenerator,
      flags.strict);
}


LocalResourceProviderDaemon::LocalResourceProviderDaemon(
    const http::URL& url,
    const string& workDir,
    const Option<string>& configDir,
    SecretGenerator* secretGenerator,
    bool strict)
  : process(new LocalResourceProviderDaemonProcess(
        url, workDir, configDir, secretGenerator, strict))
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


Future<bool> LocalResourceProviderDaemon::add(const ResourceProviderInfo& info)
{
  return dispatch(
      process.get(),
      &LocalResourceProviderDaemonProcess::add,
      info);
}


Future<bool> LocalResourceProviderDaemon::update(
    const ResourceProviderInfo& info)
{
  return dispatch(
      process.get(),
      &LocalResourceProviderDaemonProcess::update,
      info);
}


Future<Nothing> LocalResourceProviderDaemon::remove(
    const string& type,
    const string& name)
{
  return dispatch(
      process.get(),
      &LocalResourceProviderDaemonProcess::remove,
      type,
      name);
}

} // namespace internal {
} // namespace mesos {
