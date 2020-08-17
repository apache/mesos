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

using process::grpc::client::Runtime;

using process::http::authentication::Principal;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

static VolumeState createVolumeState(
    const Volume::Source::CSIVolume::StaticProvisioning& volume);


static hashset<CSIPluginContainerInfo::Service> extractServices(
    const CSIPluginInfo& plugin);


class CSIServerProcess : public process::Process<CSIServerProcess>
{
public:
  CSIServerProcess(
      const process::http::URL& _agentUrl,
      const string& _rootDir,
      SecretGenerator* _secretGenerator,
      SecretResolver* _secretResolver,
      hashmap<string, CSIPluginInfo> _pluginConfigs)
    : process::ProcessBase(process::ID::generate("csi-server")),
      agentUrl(_agentUrl),
      rootDir(_rootDir),
      secretGenerator(_secretGenerator),
      secretResolver(_secretResolver),
      pluginConfigs(_pluginConfigs) {}

  Future<Nothing> start();

  Future<string> publishVolume(const Volume::Source::CSIVolume& volume);

  Future<Nothing> unpublishVolume(
      const string& pluginName,
      const string& volumeId);

private:
  struct CSIPlugin
  {
    CSIPlugin(const string& metricsPrefix) : metrics(metricsPrefix) {}

    CSIPluginInfo info;
    Owned<ServiceManager> serviceManager;
    Owned<VolumeManager> volumeManager;
    Runtime runtime;
    csi::Metrics metrics;
  };

  // Contains the plugins loaded by the server. The key of this map is the
  // plugin name.
  hashmap<string, CSIPlugin> plugins;

  const process::http::URL agentUrl;
  const string rootDir;
  SecretGenerator* secretGenerator;
  SecretResolver* secretResolver;
  Option<string> authToken;
  hashmap<string, CSIPluginInfo> pluginConfigs;
};


Future<Nothing> CSIServerProcess::start()
{
  Future<Nothing> result = Nothing();

  // The contents of this principal are arbitrary. We choose to avoid a
  // principal with a 'value' string so that we do not unintentionally collide
  // with another real principal with restricted permissions.
  Principal principal(Option<string>::none(), {{"key", "csi-server"}});

  if (secretGenerator) {
    result = secretGenerator->generate(principal)
      .then([=](const Secret& secret) -> Future<Nothing> {
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
    });
  }

  // Initialize CSI plugins.
  vector<Future<Nothing>> initializations;

  foreachpair (const string& name, const CSIPluginInfo& info, pluginConfigs) {
    // Default-construct the plugin struct so that we have a valid runtime
    // to pass into the service manager.
    plugins.put(name, CSIPlugin("csi_plugins/" + name + "/"));

    if (info.containers_size() > 0) {
      plugins.at(name).serviceManager.reset(new ServiceManager(
          agentUrl,
          rootDir,
          info,
          extractServices(info),
          "org-apache-mesos-internal-",
          authToken,
          plugins.at(name).runtime,
          &plugins.at(name).metrics));
    } else {
      CHECK(info.endpoints_size() > 0);

      plugins.at(name).serviceManager.reset(new ServiceManager(
          info,
          extractServices(info),
          plugins.at(name).runtime,
          &plugins.at(name).metrics));
    }

    initializations.push_back(plugins.at(name).serviceManager->recover()
      .then(defer(self(), [=]() {
        CHECK(plugins.contains(name));

        return plugins.at(name).serviceManager->getApiVersion();
      }))
      .then(defer(self(), [=](const string& apiVersion) -> Future<Nothing> {
        CHECK(plugins.contains(name));

        Try<Owned<VolumeManager>> volumeManager = VolumeManager::create(
            rootDir,
            info,
            extractServices(info),
            apiVersion,
            plugins.at(name).runtime,
            plugins.at(name).serviceManager.get(),
            &plugins.at(name).metrics,
            secretResolver);

        if (volumeManager.isError()) {
          return Failure(
              "CSI server failed to create volume manager for plugin"
              " '" + info.name() + "': " + volumeManager.error());
        }

        plugins.at(name).volumeManager = std::move(volumeManager.get());

        return plugins.at(name).volumeManager->recover();
      })));
  }

  return result
    .then([=]() {
      return process::collect(initializations);
    })
    .then([=]() {
      return Nothing();
    });
}


Future<string> CSIServerProcess::publishVolume(
    const Volume::Source::CSIVolume& volume)
{
  CHECK(volume.has_static_provisioning());

  if (!plugins.contains(volume.plugin_name())) {
    return Failure("Invalid CSI plugin '" + volume.plugin_name() + "'");
  }

  return plugins.at(volume.plugin_name()).volumeManager->publishVolume(
      volume.static_provisioning().volume_id(),
      createVolumeState(volume.static_provisioning()))
    .then([=]() {
      CHECK(plugins.contains(volume.plugin_name()));

      const CSIPluginInfo& info = plugins.at(volume.plugin_name()).info;

      const string mountRootDir = info.has_target_path_root()
          ? info.target_path_root()
          : csi::paths::getMountRootDir(rootDir, info.type(), info.name());

      return csi::paths::getMountTargetPath(
          mountRootDir,
          volume.static_provisioning().volume_id());
    });
}


Future<Nothing> CSIServerProcess::unpublishVolume(
    const string& pluginName,
    const string& volumeId)
{
  if (!plugins.contains(pluginName)) {
    return Failure("Invalid CSI plugin '" + pluginName + "'");
  }

  return plugins.at(pluginName).volumeManager->unpublishVolume(volumeId);
}


VolumeState createVolumeState(
    const Volume::Source::CSIVolume::StaticProvisioning& volume)
{
  VolumeState result;
  result.set_state(VolumeState::NODE_READY);
  *result.mutable_volume_capability() = volume.volume_capability();
  *result.mutable_volume_context() = volume.volume_context();
  result.set_readonly(volume.readonly());
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
    SecretGenerator* secretGenerator,
    SecretResolver* secretResolver,
    const hashmap<string, CSIPluginInfo>& pluginConfigs)
  : process(new CSIServerProcess(
        agentUrl,
        rootDir,
        secretGenerator,
        secretResolver,
        pluginConfigs))
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

  Try<list<string>> entries = os::ls(flags.csi_plugin_config_dir.get());
  if (entries.isError()) {
    return Error(
        "Unable to list the CSI plugin configuration directory '" +
        flags.csi_plugin_config_dir.get()+ "': " + entries.error());
  }

  hashmap<std::string, CSIPluginInfo> pluginConfigs;

  foreach (const string& entry, entries.get()) {
    const string path = path::join(flags.csi_plugin_config_dir.get(), entry);

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
      return Error("JSON parse failed: " + json.error());
    }

    Try<CSIPluginInfo> parse = ::protobuf::parse<CSIPluginInfo>(json.get());
    if (parse.isError()) {
      return Error("Protobuf parse failed: " + parse.error());
    }

    const CSIPluginInfo& csiPluginConfig = parse.get();
    const string& type = csiPluginConfig.type();

    if (pluginConfigs.contains(type)) {
      LOG(ERROR) << "Multiple configurations for a CSI plugin are not allowed. "
                 << "Skipping configuration file '" << path << "' since CSI "
                 << "plugin '" << type << "' already exists";
      continue;
    }

    pluginConfigs[type] = csiPluginConfig;
  }

  if (pluginConfigs.empty()) {
    return Error(
        "No valid CSI plugin configurations found in '" +
        flags.csi_plugin_config_dir.get() + "'");
  }

  return new CSIServer(
      agentUrl,
      slave::paths::getCsiRootDir(flags.work_dir),
      secretGenerator,
      secretResolver,
      pluginConfigs);
}


Future<Nothing> CSIServer::start()
{
  started.associate(process::dispatch(process.get(), &CSIServerProcess::start));

  return started.future();
}


Future<string> CSIServer::publishVolume(
    const Volume::Source::CSIVolume& volume)
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
