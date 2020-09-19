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

#include <list>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/secret/resolver.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/sequence.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "common/validation.hpp"

#include "csi/metrics.hpp"
#include "csi/paths.hpp"
#include "csi/service_manager.hpp"
#include "csi/volume_manager.hpp"

#include "slave/csi_server.hpp"
#include "slave/flags.hpp"
#include "slave/paths.hpp"

using mesos::csi::ServiceManager;
using mesos::csi::VolumeManager;

using mesos::csi::state::VolumeState;

using process::Failure;
using process::Future;
using process::Owned;
using process::Promise;

using process::grpc::client::Runtime;

using process::http::authentication::Principal;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

constexpr char DEFAULT_CSI_CONTAINER_PREFIX[] = "mesos-internal-csi-";

static VolumeState createVolumeState(const Volume& volume);


static hashset<CSIPluginContainerInfo::Service> extractServices(
    const CSIPluginInfo& plugin);


class CSIServerProcess : public process::Process<CSIServerProcess>
{
public:
  CSIServerProcess(
      const process::http::URL& _agentUrl,
      const string& _rootDir,
      const string& _pluginConfigDir,
      SecretGenerator* _secretGenerator,
      SecretResolver* _secretResolver)
    : process::ProcessBase(process::ID::generate("csi-server")),
      agentUrl(_agentUrl),
      rootDir(_rootDir),
      pluginConfigDir(_pluginConfigDir),
      secretGenerator(_secretGenerator),
      secretResolver(_secretResolver) {}

  Future<Nothing> start(const SlaveID& _agentId);

  Future<string> publishVolume(const Volume& volume);

  Future<Nothing> unpublishVolume(
      const string& pluginName,
      const string& volumeId);

private:
  struct CSIPlugin
  {
    CSIPlugin(
        const CSIPluginInfo& _info,
        const string& metricsPrefix)
      : info(_info),
        metrics(metricsPrefix) {}

    CSIPluginInfo info;
    Owned<ServiceManager> serviceManager;
    Owned<VolumeManager> volumeManager;
    Runtime runtime;
    csi::Metrics metrics;

    // CSI plugins are initialized lazily. When a publish/unpublish call is
    // received for a plugin which is not yet initialized, this promise is used
    // to perform the call after initialization is complete.
    Promise<Nothing> initialized;
  };

  // Attempts to load configuration for a plugin with the specified name and
  // then initializes the plugin. If no name is specified, then all
  // configurations found in the plugin config directory are loaded.
  Try<Nothing> initializePlugin(const Option<string>& name = None());

  // Contains the plugins loaded by the server. The key of this map is the
  // plugin name.
  hashmap<string, CSIPlugin> plugins;

  const process::http::URL agentUrl;
  Option<SlaveID> agentId;
  const string rootDir;
  const string pluginConfigDir;
  SecretGenerator* secretGenerator;
  SecretResolver* secretResolver;
  Option<string> authToken;
};


Try<Nothing> CSIServerProcess::initializePlugin(const Option<string>& name)
{
  if (name.isSome()) {
    CHECK(!plugins.contains(name.get()));
  }

  Try<list<string>> entries = os::ls(pluginConfigDir);
  if (entries.isError()) {
    return Error(
        "Unable to list the CSI plugin configuration directory '" +
        pluginConfigDir + "': " + entries.error());
  }

  // We are either looking for one specific plugin (if `name` is SOME), or we
  // are loading all configs we find (if `name` is NONE). First, we populate
  // `pluginConfigs` with one or more valid configurations. Then, we will
  // initialize the plugin(s) based on the configuration(s) found.
  hashmap<string, CSIPluginInfo> pluginConfigs;

  foreach (const string& entry, entries.get()) {
    const string path = path::join(pluginConfigDir, entry);

    // Ignore directory entries.
    if (os::stat::isdir(path)) {
      continue;
    }

    Try<string> read = os::read(path);
    if (read.isError()) {
      // In case of an error we log and skip to the next entry.
      LOG(ERROR) << "Failed to read CSI plugin configuration file '"
                 << path << "': " << read.error();

      continue;
    }

    Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
    if (json.isError()) {
      return Error("JSON parse of '" + path + "' failed: " + json.error());
    }

    Try<CSIPluginInfo> parse = ::protobuf::parse<CSIPluginInfo>(json.get());
    if (parse.isError()) {
      return Error("Protobuf parse of '" + path + "' failed: " + parse.error());
    }

    const CSIPluginInfo& csiPluginConfig = parse.get();
    const string& type = csiPluginConfig.type();

    if (pluginConfigs.contains(type)) {
      LOG(ERROR) << "Multiple configurations for a CSI plugin are not allowed. "
                 << "Skipping configuration file '" << path << "' since CSI "
                 << "plugin '" << type << "' already exists";
      continue;
    }

    if (name.isNone() || name.get() == type) {
      pluginConfigs[type] = csiPluginConfig;

      if (name.isSome()) {
        break;
      }
    }
  }

  if (pluginConfigs.empty()) {
    return Error(
        "No valid CSI plugin configurations found in '" +
        pluginConfigDir + "'");
  }

  foreachpair (const string& _name, const CSIPluginInfo& info, pluginConfigs) {
    // Default-construct the plugin struct so that we have a valid runtime
    // to pass into the service manager.
    plugins.put(_name, CSIPlugin(info, "csi_plugins/" + _name + "/"));

    CSIPlugin& plugin = plugins.at(_name);

    if (info.containers_size() > 0) {
      CHECK_SOME(agentId);

      plugin.serviceManager.reset(new ServiceManager(
          agentId.get(),
          agentUrl,
          rootDir,
          info,
          extractServices(info),
          DEFAULT_CSI_CONTAINER_PREFIX,
          authToken,
          plugin.runtime,
          &plugin.metrics));
    } else {
      CHECK(info.endpoints_size() > 0);

      plugin.serviceManager.reset(new ServiceManager(
          info,
          extractServices(info),
          plugin.runtime,
          &plugin.metrics));
    }

    plugin.initialized.associate(
        plugin.serviceManager->recover()
          .then(defer(self(), [=]() {
            CHECK(plugins.contains(_name));

            return plugins.at(_name).serviceManager->getApiVersion();
          }))
          .then(defer(self(), [=](const string& apiVersion) -> Future<Nothing> {
            CHECK(plugins.contains(_name));

            Try<Owned<VolumeManager>> volumeManager = VolumeManager::create(
                rootDir,
                info,
                extractServices(info),
                apiVersion,
                plugins.at(_name).runtime,
                plugins.at(_name).serviceManager.get(),
                &plugins.at(_name).metrics,
                secretResolver);

            if (volumeManager.isError()) {
              return Failure(
                  "CSI server failed to create volume manager for plugin"
                  " '" + info.name() + "': " + volumeManager.error());
            }

            plugins.at(_name).volumeManager = std::move(volumeManager.get());

            return plugins.at(_name).volumeManager->recover();
          }))
          .onAny(defer(self(), [=](const Future<Nothing>& future) {
            if (!future.isReady()) {
              plugins.erase(_name);

              LOG(ERROR)
                << "CSI server failed to initialize plugin '" << _name << "': "
                << (future.isFailed() ? future.failure() : "discarded");
            } else {
              LOG(INFO)
                << "CSI server successfully initialized plugin '"
                << _name << "'";
            }
          })));
  }

  return Nothing();
}


Future<Nothing> CSIServerProcess::start(const SlaveID& _agentId)
{
  // NOTE: It's possible that the agent receives multiple
  // `SlaveRegisteredMessage`s and detects a disconnection in between.
  // In that case, `start` will be called multiple times from
  // `Slave::registered`.
  if (agentId.isSome()) {
    CHECK_EQ(agentId.get(), _agentId)
      << "Cannot start CSI server with agent ID " << _agentId
      << " (expected: " << agentId.get() << ")";

    return Nothing();
  }

  agentId = _agentId;

  Future<Nothing> result = Nothing();

  if (secretGenerator) {
    // The contents of this principal are arbitrary. We choose to avoid a
    // principal with a 'value' string so that we do not unintentionally collide
    // with another real principal with restricted permissions.
    Principal principal(
        Option<string>::none(),
        {{"cid_prefix", DEFAULT_CSI_CONTAINER_PREFIX}});

    result = secretGenerator->generate(principal)
      .then(defer(self(), [=](const Secret& secret) -> Future<Nothing> {
        Option<Error> error = common::validation::validateSecret(secret);
        if (error.isSome()) {
          return Failure(
              "CSI server failed to validate generated secret: " +
              error->message);
        }

        if (secret.type() != Secret::VALUE) {
          return Failure(
              "CSI server expecting generated secret to be of VALUE type "
              "instead of " + stringify(secret.type()) + " type; " +
              "only VALUE type secrets are supported at this time");
        }

        CHECK(secret.has_value());

        authToken = secret.value().data();

        return Nothing();
    }));
  }

  return result
    .then(defer(self(), [=]() -> Future<Nothing> {
      // Load all CSI plugin configurations found.
      // NOTE: `initializePlugin()` requires that the `authToken` has already
      // been set, so the order of these continuations matters.
      Try<Nothing> init = initializePlugin();
      if (init.isError()) {
        return Failure(
            "CSI server failed to initialize CSI plugins: " + init.error());
      }

      return Nothing();
    }));
}


Future<string> CSIServerProcess::publishVolume(const Volume& volume)
{
  CHECK(volume.has_source() &&
        volume.source().has_type() &&
        volume.source().type() == Volume::Source::CSI_VOLUME);

  CHECK(volume.source().has_csi_volume() &&
        volume.source().csi_volume().has_static_provisioning());

  const Volume::Source::CSIVolume& csiVolume = volume.source().csi_volume();

  const string& name = csiVolume.plugin_name();

  if (!plugins.contains(name)) {
    // This will attempt to load the plugin's configuration, initialize the
    // plugin, and insert it into the `plugins` map.
    Try<Nothing> pluginInit = initializePlugin(name);
    if (pluginInit.isError()) {
      return Failure(
          "Failed to initialize CSI plugin '" +
          name + "': " + pluginInit.error());
    }
  }

  CHECK(plugins.contains(name));

  return plugins.at(name).initialized.future()
    .then(defer(self(), [=]() {
      CHECK(plugins.contains(name));

      return plugins.at(name).volumeManager->publishVolume(
          csiVolume.static_provisioning().volume_id(),
          createVolumeState(volume));
      }))
    .then(defer(self(), [=]() {
      CHECK(plugins.contains(name));

      const CSIPluginInfo& info = plugins.at(csiVolume.plugin_name()).info;

      const string mountRootDir = info.has_target_path_root()
          ? info.target_path_root()
          : csi::paths::getMountRootDir(rootDir, info.type(), info.name());

      return csi::paths::getMountTargetPath(
          mountRootDir,
          csiVolume.static_provisioning().volume_id());
    }));
}


Future<Nothing> CSIServerProcess::unpublishVolume(
    const string& pluginName,
    const string& volumeId)
{
  if (!plugins.contains(pluginName)) {
    // This will attempt to load the plugin's configuration, initialize the
    // plugin, and insert it into the `plugins` map.
    Try<Nothing> pluginInit = initializePlugin(pluginName);
    if (pluginInit.isError()) {
      return Failure(
          "Failed to initialize CSI plugin '" +
          pluginName + "': " + pluginInit.error());
    }
  }

  CHECK(plugins.contains(pluginName));

  return plugins.at(pluginName).initialized.future()
    .then(defer(self(), [=]() {
      return plugins.at(pluginName).volumeManager->unpublishVolume(volumeId);
    }));
}


VolumeState createVolumeState(const Volume& volume)
{
  const Volume::Source::CSIVolume::StaticProvisioning& staticProvisioning =
    volume.source().csi_volume().static_provisioning();

  VolumeState result;
  result.set_state(VolumeState::NODE_READY);
  *result.mutable_volume_capability() = staticProvisioning.volume_capability();
  *result.mutable_volume_context() = staticProvisioning.volume_context();
  result.set_readonly(volume.mode() == Volume::RO);
  result.set_pre_provisioned(true);

  return result;
}


hashset<CSIPluginContainerInfo::Service> extractServices(
    const CSIPluginInfo& plugin)
{
  hashset<CSIPluginContainerInfo::Service> result;

  if (plugin.containers_size() > 0) {
    foreach (const CSIPluginContainerInfo& container, plugin.containers()) {
      for (int i = 0; i < container.services_size(); ++i) {
        result.insert(container.services(i));
      }
    }
  } else {
    CHECK(plugin.endpoints_size() > 0);

    foreach (const CSIPluginEndpoint& endpoint, plugin.endpoints()) {
      result.insert(endpoint.csi_service());
    }
  }

  return result;
}


CSIServer::CSIServer(
    const process::http::URL& agentUrl,
    const string& rootDir,
    const string& pluginConfigDir,
    SecretGenerator* secretGenerator,
    SecretResolver* secretResolver)
  : process(new CSIServerProcess(
        agentUrl,
        rootDir,
        pluginConfigDir,
        secretGenerator,
        secretResolver))
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


CSIServer::~CSIServer()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Try<Owned<CSIServer>> CSIServer::create(
    const Flags& flags,
    const process::http::URL& agentUrl,
    SecretGenerator* secretGenerator,
    SecretResolver* secretResolver)
{
  if (!strings::contains(flags.isolation, "volume/csi")) {
    return Error("Missing required isolator 'volume/csi'");
  }

  if (flags.csi_plugin_config_dir.isNone() ||
      flags.csi_plugin_config_dir->empty()) {
    return Error("Missing required '--csi_plugin_config_dir' flag");
  }

  if (!os::exists(flags.csi_plugin_config_dir.get())) {
    return Error(
        "The CSI plugin configuration directory '" +
        flags.csi_plugin_config_dir.get() + "' does not exist");
  }

  return new CSIServer(
      agentUrl,
      slave::paths::getCsiRootDir(flags.work_dir),
      flags.csi_plugin_config_dir.get(),
      secretGenerator,
      secretResolver);
}


Future<Nothing> CSIServer::start(const SlaveID& agentId)
{
  started.associate(process::dispatch(
      process.get(),
      &CSIServerProcess::start,
      agentId));

  return started.future();
}


Future<string> CSIServer::publishVolume(const Volume& volume)
{
  return started.future()
    .then(process::defer(
        process.get(),
        &CSIServerProcess::publishVolume,
        volume));
}


Future<Nothing> CSIServer::unpublishVolume(
    const string& pluginName,
    const string& volumeId)
{
  return started.future()
    .then(process::defer(
        process.get(),
        &CSIServerProcess::unpublishVolume,
        pluginName,
        volumeId));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
