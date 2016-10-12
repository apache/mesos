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

#include <stout/os.hpp>
#include <stout/protobuf.hpp>

#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include "slave/containerizer/mesos/isolators/network/cni/spec.hpp"
#include "slave/containerizer/mesos/isolators/network/cni/plugins/port_mapper/port_mapper.hpp"

using std::string;
using std::vector;

using process::Future;
using process::Owned;

using mesos::NetworkInfo;

using mesos::internal::slave::cni::spec::PluginError;

namespace mesos {
namespace internal {
namespace slave {
namespace cni {

Try<Owned<PortMapper>, PluginError> PortMapper::create(const string& _cniConfig)
{
  Option<string> cniCommand = os::getenv("CNI_COMMAND");
  if (cniCommand.isNone()) {
    return PluginError(
        "Unable to find environment variable 'CNI_COMMAND'",
        ERROR_BAD_ARGS);
  }

  // 'CNI_CONTAINERID' is optional.
  Option<string> cniContainerId = os::getenv("CNI_CONTAINERID");

  Option<string> cniNetNs = os::getenv("CNI_NETNS");
  if (cniNetNs.isNone()) {
    return PluginError(
        "Unable to find environment variable 'CNI_NETNS'",
        ERROR_BAD_ARGS);
  }

  Option<string> cniIfName = os::getenv("CNI_IFNAME");
  if (cniIfName.isNone()) {
    return PluginError(
        "Unable to find environment variable 'CNI_IFNAME'",
        ERROR_BAD_ARGS);
  }

  // 'CNI_ARGS' is optional.
  Option<string> cniArgs = os::getenv("CNI_ARGS");

  Option<string> cniPath = os::getenv("CNI_PATH");
  if (cniPath.isNone()) {
    return PluginError(
        "Unable to find environment variable 'CNI_PATH'",
        ERROR_BAD_ARGS);
  }

  // Verify the CNI config for this plugin.
  Try<JSON::Object> cniConfig = JSON::parse<JSON::Object>(_cniConfig);
  if (cniConfig.isError()) {
    return PluginError(cniConfig.error(), ERROR_BAD_ARGS);
  }

  // TODO(jieyu): Validate 'cniVersion' and 'type'.

  Result<JSON::String> name = cniConfig->find<JSON::String>("name");
  if (!name.isSome()) {
    return PluginError(
        "Failed to get the required field 'name': " +
        (name.isError() ? name.error() : "Not found"),
        ERROR_BAD_ARGS);
  }

  Result<JSON::String> chain = cniConfig->find<JSON::String>("chain");
  if (!chain.isSome()) {
    return PluginError(
        "Failed to get the required field 'chain': " +
        (chain.isError() ? chain.error() : "Not found"),
        ERROR_BAD_ARGS);
  }

  vector<string> excludeDevices;

  Result<JSON::Array> _excludeDevices =
    cniConfig->find<JSON::Array>("excludeDevices");

  if (_excludeDevices.isError()) {
    return PluginError(
        "Failed to parse field 'excludeDevices': " +
        _excludeDevices.error(),
        ERROR_BAD_ARGS);
  } else if (_excludeDevices.isSome()) {
    foreach (const JSON::Value& value, _excludeDevices->values) {
      if (!value.is<JSON::String>()) {
        return PluginError(
            "Failed to parse 'excludeDevices' list. "
            "The excluded device needs to be a string",
            ERROR_BAD_ARGS);
      }

      excludeDevices.push_back(value.as<JSON::String>().value);
    }
  }

  // While the 'args' field is optional in the CNI spec it is critical
  // to the port-mapper plugin to learn of any port-mappings that the
  // framework might have requested for this container.
  Result<JSON::Object> args = cniConfig->find<JSON::Object>("args");
  if (!args.isSome()) {
    return PluginError(
        "Failed to get the required field 'args': " +
        (args.isError() ? args.error() : "Not found"),
        ERROR_BAD_ARGS);
  }

  // NOTE: We can't directly use `find` to check for 'network_info'
  // within the 'args' dict, since 'org.apache.mesos' will be treated
  // as a path by `find` instead of a key. So we need to retrieve the
  // 'org.apache.mesos' key first and then use it to find
  // 'network_info'.
  Result<JSON::Object> mesos = args->at<JSON::Object>("org.apache.mesos");
  if (!mesos.isSome()) {
    return PluginError(
        "Failed to get the field 'args{org.apache.mesos}': " +
        (mesos.isError() ? mesos.error() : "Not found"),
        ERROR_BAD_ARGS);
  }

  Result<JSON::Object> _networkInfo = mesos->find<JSON::Object>("network_info");
  if (!_networkInfo.isSome()) {
    return PluginError(
        "Failed to get the field 'args{org.apache.mesos}{network_info}': " +
        (_networkInfo.isError() ? _networkInfo.error() : "Not found"),
        ERROR_BAD_ARGS);
  }

  Try<NetworkInfo> networkInfo =
    ::protobuf::parse<NetworkInfo>(_networkInfo.get());

  if (networkInfo.isError()) {
    return PluginError(
        "Unable to parse `NetworkInfo`: " + networkInfo.error(),
        ERROR_BAD_ARGS);
  }

  // The port-mapper should always be used in conjunction with another
  // 'delegate' CNI plugin.
  Result<JSON::Object> _delegateConfig =
    cniConfig->find<JSON::Object>("delegate");

  if (!_delegateConfig.isSome()) {
    return PluginError(
        "Failed to get the required field 'delegate'" +
        (_delegateConfig.isError() ? _delegateConfig.error() : "Not found"),
        ERROR_BAD_ARGS);
  }

  // TODO(jieyu): Validate that 'cniVersion' and 'name' exist in
  // 'delegate' as it should be a valid CNI config JSON.

  // Make sure the 'delegate' plugin exists.
  Result<JSON::String> delegatePlugin =
    _delegateConfig->find<JSON::String>("type");

  if (!delegatePlugin.isSome()) {
    return PluginError(
        "Failed to get the delegate plugin 'type'" +
        (delegatePlugin.isError() ? delegatePlugin.error() : "Not found"),
        ERROR_BAD_ARGS);
  }

  if (os::which(delegatePlugin->value, cniPath.get()).isNone()) {
    return PluginError(
        "Could not find the delegate plugin '" + delegatePlugin->value +
        "' in '" + cniPath.get() + "'",
        ERROR_BAD_ARGS);
  }

  // Add the 'name' and 'args' field to the 'delegate' config.
  JSON::Object delegateConfig(_delegateConfig.get());
  delegateConfig.values["name"] = name.get();
  delegateConfig.values["args"] = args.get();

  return Owned<PortMapper>(
      new PortMapper(
          cniCommand.get(),
          cniContainerId,
          cniNetNs.get(),
          cniIfName.get(),
          cniArgs,
          cniPath.get(),
          networkInfo.get(),
          delegatePlugin->value,
          delegateConfig,
          chain->value,
          excludeDevices));
}


Try<Option<string>, PluginError> PortMapper::execute()
{
  return None();
}


Result<spec::NetworkInfo> PortMapper::delegate(const string& command)
{
  return None();
}

} // namespace cni {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
