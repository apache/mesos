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

#include <process/collect.hpp>
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
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "common/http.hpp"
#include "common/validation.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/local.hpp"

namespace http = process::http;

using std::list;
using std::string;
using std::vector;

using process::await;
using process::defer;
using process::dispatch;
using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::ProcessBase;
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
    Option<string> authToken;

    // The `version` is used to check if `provider` holds a resource
    // provider instance that is in sync with the current config.
    id::UUID version;
    Owned<LocalResourceProvider> provider;

    // If set, it means that the resource provider is being removed, and the
    // future would be completed once the removal is done. Note that this object
    // will be destructed as soon as the future is ready.
    Option<Future<Nothing>> removing;
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

  Future<Nothing> cleanupContainers(
      const ResourceProviderInfo& info,
      const Option<string>& authToken);

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
    foreachpair (const string& name,
                 const ProviderData& data,
                 providers[type]) {
      if (data.removing.isSome()) {
        continue;
      }

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
    const ProviderData& data = providers[info.type()].at(info.name());

    if (data.removing.isSome()) {
      return Failure(
          "Failed to add resource provider with type '" + info.type() +
          "' and name '" + info.name() + "' as a removal is still in progress");
    }

    return data.info == info;
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

  if (data.removing.isSome()) {
    return Failure(
        "Failed to update resource provider with type '" + info.type() +
        "' and name '" + info.name() + "' as a removal is still in progress");
  }

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

  ProviderData& data = providers[type].at(name);

  // Return the same future if it is being removed for idempotency.
  if (data.removing.isSome() && data.removing->isPending()) {
    return data.removing.get();
  }

  // Destruct the resource provider instance to stop its container daemons, then
  // do a best-effort cleanup of the standalone containers launched by the
  // removed resource provider.
  // TODO(chhsiao): This is not ideal since the daemon does not know how to
  // perform resource-provider-specific cleanups. We should refactor this into a
  // `LocalResourceProvider::stop` virtual function. However this also means
  // that we need to ensure that we must have a resource provider instance with
  // a running actor to invoke `stop`. Given that `launch` is asynchronous, we
  // need to carefully redesign the state machine, and the semantics of the
  // `{ADD,UPDATE,REMOVE}_RESOURCE_PROVIDER_CONFIG` API calls might be affected.
  // In addition, we should consider how to reconcile orphaned containers.
  data.provider.reset();
  data.removing = cleanupContainers(data.info, data.authToken)
    .then(defer(self(), [this, type, name]() -> Future<Nothing> {
      Try<Nothing> rm = os::rm(providers[type].at(name).path);
      if (rm.isError()) {
        return Failure(
            "Failed to remove config file '" + providers[type].at(name).path +
            "': " + rm.error());
      }

      providers[type].erase(name);
      return Nothing();
    }));

  return data.removing.get();
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
  CHECK(providers[type].contains(name));

  ProviderData& data = providers[type].at(name);
  CHECK(data.removing.isNone());

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

  // If the resource provider config is being removed, nothing needs to be done.
  if (data.removing.isSome()) {
    return Nothing();
  }

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

  data.authToken = authToken;
  data.provider = std::move(provider.get());

  return Nothing();
}


// Generates a secret for local resource provider authentication if needed.
Future<Option<string>> LocalResourceProviderDaemonProcess::generateAuthToken(
    const ResourceProviderInfo& info)
{
  if (secretGenerator == nullptr) {
    return None();
  }

  return secretGenerator->generate(LocalResourceProvider::principal(info))
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


Future<Nothing> LocalResourceProviderDaemonProcess::cleanupContainers(
    const ResourceProviderInfo& info,
    const Option<string>& authToken)
{
  const Principal principal = LocalResourceProvider::principal(info);
  CHECK(principal.claims.contains("cid_prefix"));

  const string& cidPrefix = principal.claims.at("cid_prefix");

  LOG(INFO) << "Cleaning up containers prefixed by '" << cidPrefix << "'";

  // TODO(chhsiao): Consider using a more reliable way to get the v1 endpoint.
  http::URL agentUrl = url;
  agentUrl.path = Path(url.path).dirname();

  http::Headers headers{{"Accept", stringify(ContentType::PROTOBUF)}};
  if (authToken.isSome()) {
    headers["Authorization"] = "Bearer " + authToken.get();
  }

  agent::Call call;
  call.set_type(agent::Call::GET_CONTAINERS);
  call.mutable_get_containers()->set_show_nested(false);
  call.mutable_get_containers()->set_show_standalone(true);

  return http::post(
      agentUrl,
      headers,
      serialize(ContentType::PROTOBUF, evolve(call)),
      stringify(ContentType::PROTOBUF))
    .then(defer(self(), [=](
        const http::Response& httpResponse) -> Future<Nothing> {
      if (httpResponse.status != http::OK().status) {
        return Failure(
            "Failed to get containers: Unexpected response '" +
            httpResponse.status + "' (" + httpResponse.body + ")");
      }

      Try<v1::agent::Response> v1Response = deserialize<v1::agent::Response>(
          ContentType::PROTOBUF, httpResponse.body);
      if (v1Response.isError()) {
        return Failure("Failed to get containers: " + v1Response.error());
      }

      vector<Future<Nothing>> futures;

      agent::Response response = devolve(v1Response.get());
      foreach (const agent::Response::GetContainers::Container& container,
               response.get_containers().containers()) {
        const ContainerID& containerId = container.container_id();

        if (!strings::startsWith(containerId.value(), cidPrefix)) {
          continue;
        }

        // NOTE: We skip containers that are not actually running by checking
        // their `executor_pid`s to avoid `ESRCH` errors or killing an arbitrary
        // process. But we might skip the ones that are being launched as well.
        if (!container.has_container_status() ||
            !container.container_status().has_executor_pid()) {
          LOG(WARNING)
            << "Skipped killing container '" << containerId
            << "' because it is not running";

          continue;
        }

        agent::Call call;
        call.set_type(agent::Call::KILL_CONTAINER);
        call.mutable_kill_container()->mutable_container_id()
          ->CopyFrom(containerId);

        LOG(INFO) << "Killing container '" << containerId << "'";

        futures.push_back(http::post(
            agentUrl,
            headers,
            serialize(ContentType::PROTOBUF, evolve(call)),
            stringify(ContentType::PROTOBUF))
          .then(defer(self(), [=](
              const http::Response& response) -> Future<Nothing> {
            if (response.status == http::NotFound().status) {
              LOG(WARNING)
                << "Skipped waiting for container '" << containerId
                << "' because it no longer exists";

              return Nothing();
            }

            if (response.status != http::OK().status) {
              return Failure(
                  "Failed to kill container '" + stringify(containerId) +
                  "': Unexpected response '" + response.status + "' (" +
                  response.body + ")");
            }

            LOG(INFO) << "Waiting for container '" << containerId << "'";

            agent::Call call;
            call.set_type(agent::Call::WAIT_CONTAINER);
            call.mutable_wait_container()->mutable_container_id()
              ->CopyFrom(containerId);

            return http::post(
                agentUrl,
                headers,
                serialize(ContentType::PROTOBUF, evolve(call)),
                stringify(ContentType::PROTOBUF))
              .then([containerId](
                  const http::Response& response) -> Future<Nothing> {
                if (response.status != http::OK().status &&
                    response.status != http::NotFound().status) {
                  return Failure(
                      "Failed to wait for container '" +
                      stringify(containerId) + "': Unexpected response '" +
                      response.status + "' (" + response.body + ")");
                }

                return Nothing();
              });
          })));
      }

      // We use await here to do a best-effort cleanup.
      return await(futures)
        .then([cidPrefix](
            const vector<Future<Nothing>>& futures) -> Future<Nothing> {
          foreach (const Future<Nothing>& future, futures) {
            if (!future.isReady()) {
              return Failure(
                  "Failed to clean up containers prefixed by '" + cidPrefix +
                  "': " + stringify(futures));
            }
          }

          return Nothing();
        });
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
