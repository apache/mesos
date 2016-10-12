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

namespace io = process::io;

using std::cerr;
using std::endl;
using std::map;
using std::string;
using std::tuple;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Subprocess;

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
  Result<JSON::String> _delegatePlugin =
    _delegateConfig->find<JSON::String>("type");

  if (!_delegatePlugin.isSome()) {
    return PluginError(
        "Failed to get the delegate plugin 'type'" +
        (_delegatePlugin.isError() ? _delegatePlugin.error() : "Not found"),
        ERROR_BAD_ARGS);
  }

  Option<string> delegatePlugin = os::which(
      _delegatePlugin->value,
      cniPath.get());

  if (delegatePlugin.isNone()) {
    return PluginError(
        "Could not find the delegate plugin '" + _delegatePlugin->value +
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
          delegatePlugin.get(),
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
  map<std::string, std::string> environment;

  environment["CNI_COMMAND"] = command;
  environment["CNI_IFNAME"] = cniIfName;
  environment["CNI_NETNS"] = cniNetNs;
  environment["CNI_PATH"] = cniPath;

  if (cniContainerId.isSome()) {
    environment["CNI_CONTAINERID"] = cniContainerId.get();
  }

  if (cniArgs.isSome()) {
    environment["CNI_ARGS"] = cniArgs.get();
  }

  // Some CNI plugins need to run "iptables" to set up IP Masquerade,
  // so we need to set the "PATH" environment variable so that the
  // plugin can locate the "iptables" executable file.
  Option<string> value = os::getenv("PATH");
  if (value.isSome()) {
    environment["PATH"] = value.get();
  } else {
    environment["PATH"] =
      "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
  }

  Try<string> temp = os::mktemp();
  if (temp.isError()) {
    return Error("Failed to create the temp file: " + temp.error());
  }

  Try<Nothing> write = os::write(
      temp.get(),
      stringify(delegateConfig));

  if (write.isError()) {
    os::rm(temp.get());
    return Error("Failed to write the temp file: " + write.error());
  }

  Try<Subprocess> s = process::subprocess(
      delegatePlugin,
      {delegatePlugin},
      Subprocess::PATH(temp.get()),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      environment);

  if (s.isError()) {
    return Error(
        "Failed to exec the '" + delegatePlugin +
        "' subprocess: " + s.error());
  }

  auto result = await(
      s->status(),
      io::read(s->out().get()),
      io::read(s->err().get()));

  result.await();

  // At this point the subprocess has completed and hence we need to
  // remove the `temp` file that we created.
  os::rm(temp.get());

  if (!result.isReady()) {
    return Error(
        "Failed to wait for exec of delegate CNI plugin '" +
        delegatePlugin + "': " +
        (result.isDiscarded() ? "discarded" : result.failure()));
  }

  Future<Option<int>> status = std::get<0>(result.get());
  if (!status.isReady()) {
    return Error(
        "Failed to get the exit status of the delegate CNI plugin '" +
        delegatePlugin + "' subprocess: " +
        (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Error(
        "Failed to reap the delegate CNI plugin '" +
        delegatePlugin + "' subprocess");
  }

  // CNI plugin will print result or error to stdout.
  Future<string> output = std::get<1>(result.get());
  if (!output.isReady()) {
    return Error(
        "Failed to read stdout from the delegate CNI plugin '" +
        delegatePlugin + "' subprocess: " +
        (output.isFailed() ? output.failure() : "discarded"));
  }

  // We are reading stderr of the plugin since any log messages from
  // the CNI plugin would be thrown on stderr. This can be useful for
  // debugging issues when the plugin throws a `spec::Error`.
  Future<string> err = std::get<2>(result.get());
  if (!err.isReady()) {
    return Error(
        "Failed to read STDERR from the delegate CNI plugin '" +
        delegatePlugin + "' subprocess: " +
        (err.isFailed() ? err.failure() : "discarded"));
  }

  if (status.get() != 0) {
    cerr << "Delegate plugin reported error: " << err.get() << endl;

    return Error(
        "The delegate CNI plugin '" + delegatePlugin +
        "' return status " + stringify(status->get()) +
        ". Could not attach container: " + output.get());
  }

  if (command == spec::CNI_CMD_ADD) {
    // Parse the output of CNI plugin.
    Try<spec::NetworkInfo> parse = spec::parseNetworkInfo(output.get());
    if (parse.isError()) {
      return Error(
          "Failed to parse the output of the delegate CNI plugin '" +
          delegatePlugin + "': " + parse.error());
    }

    return parse.get();
  }

  // For `spec::CNI_CMD_DEL` CNI plugins don't return a result. They
  // will report an error if any, which should be captured above.
  return None();
}

} // namespace cni {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
