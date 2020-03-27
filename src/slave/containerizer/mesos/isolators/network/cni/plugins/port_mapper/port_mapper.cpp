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
#include <stout/stringify.hpp>

#include <stout/os/exec.hpp>
#include <stout/os/which.hpp>

#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include "slave/containerizer/mesos/isolators/network/cni/plugins/port_mapper/port_mapper.hpp"

namespace io = process::io;

using std::cerr;
using std::endl;
using std::map;
using std::string;
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

  // NOTE: While CNI spec allows 'CNI_CONTAINERID' to be optional, the
  // plugin uses the 'CNI_CONTAINERID' to tag iptable rules so that
  // we can delete the rules without IP address information (which
  // will not be available during DEL). Hence, making this
  // variable required.
  Option<string> cniContainerId = os::getenv("CNI_CONTAINERID");
  if (cniContainerId.isNone()) {
    return PluginError(
        "Unable to find environment variable 'CNI_CONTAINERID'",
        ERROR_BAD_ARGS);
  }

  Option<string> cniNetNs = os::getenv("CNI_NETNS");
  if (cniNetNs.isNone() && cniCommand.get() != spec::CNI_CMD_DEL) {
    return PluginError(
        "Unable to find environment variable 'CNI_NETNS' for "
        "non-'" + stringify(spec::CNI_CMD_DEL) + "' command",
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
  // framework might have requested for this container when this plugin
  // is called in the Mesos context. However, to make the port-mapper
  // plugin more generic rather than Mesos specific, we will create a
  // fake 'args` field if it is not filled by the caller.
  Result<JSON::Object> args = cniConfig->find<JSON::Object>("args");
  if (args.isError()) {
    return PluginError(
        "Failed to get the field 'args': " + args.error(), ERROR_BAD_ARGS);
  } else if (args.isNone()) {
    JSON::Object _args;
    JSON::Object mesos;
    mesos.values["network_info"] = JSON::Object();
    _args.values["org.apache.mesos"] = mesos;
    args = _args;
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
          cniContainerId.get(),
          cniNetNs,
          cniIfName.get(),
          cniArgs,
          cniPath.get(),
          networkInfo.get(),
          delegatePlugin.get(),
          delegateConfig,
          chain->value,
          excludeDevices));
}


string PortMapper::getIptablesRuleTag()
{
  return "container_id: " + cniContainerId;
}


string PortMapper::getIptablesRule(
    const net::IP& ip,
    const mesos::NetworkInfo::PortMapping& portMapping)
{
  string devices;

  // Get list of devices to exclude.
  if (!excludeDevices.empty()) {
    foreach (const string& device, excludeDevices) {
      devices = "! -i " + device + " ";
    }
  }

  const string protocol = portMapping.has_protocol()
    ? strings::lower(portMapping.protocol())
    : "tcp";

  // Iptables DNAT rule representing a specific port-mapping.
  return strings::format(
      " %s %s -p %s -m %s"
      " --dport %d -j DNAT --to-destination %s:%d"
      " -m comment --comment \"%s\"",
      chain,
      devices,
      protocol,
      protocol,
      portMapping.host_port(),
      stringify(ip),
      portMapping.container_port(),
      getIptablesRuleTag()).get();
}


Try<Nothing> PortMapper::addPortMapping(
    const net::IP& ip,
    const mesos::NetworkInfo::PortMapping& portMapping)
{
  Try<string> iptablesRule = getIptablesRule(ip, portMapping);
  if (iptablesRule.isError()) {
    return Error(iptablesRule.error());
  }

  // Run the following iptables script to install the DNAT rule under
  // the specified chain.
  string script = strings::format(
      R"~(
      #!/bin/sh
      exec 1>&2
      set -x

      # NOTE: We need iptables 1.4.20 and higher for the commands to
      # work. We use the '-w' flag with the iptables command to ensure
      # that iptables command are executed atomically. This flag is
      # available starting iptables 1.4.20.
      #
      # Check if the `chain` exists in the iptable. If it does not
      # exist go ahead and install the chain in the iptables NAT
      # table.
      iptables -w -n -t nat --list %s
      if [ $? -ne 0 ]; then
        # NOTE: When we create the chain, there is a possibility of a
        # race due to which a container launch can fail. This can
        # happen specifically when two containers are launched with
        # port-mapping with the same iptables chain and the chain does
        # not exist. In this scenario, there might be a race for the
        # chain creation with only one of the containers succeeding.
        # iptables, unfortunately, does not allow locks to be acquired
        # outside the iptables process and hence there is no way to
        # avoid this race. This event itself should be quite rare
        # since it can happen only when the chain is created the first
        # time and two commands for creation of the chain are executed
        # simultaneously.
        (iptables -w -t nat -N %s || exit 1)

        # Once the chain has been installed add a rule in the PREROUTING
        # chain to jump to this chain for any packets that are
        # destined to a local address.
        (iptables -w -t nat -A PREROUTING \
        -m addrtype --dst-type LOCAL -j %s || exit 1)

        # For locally generated packets we need a rule in the OUTPUT
        # chain as well, since locally generated packets directly hit
        # the output CHAIN, bypassing PREROUTING.
        (iptables -w -t nat -A OUTPUT \
        ! -d 127.0.0.0/8 -m addrtype \
        --dst-type LOCAL -j %s || exit 1)
      fi

      # Within the `chain` go ahead and install the DNAT rule, if it
      # does not exist.
      (iptables -w -t nat -C %s || iptables -w -t nat -A %s))~",
      chain,
      chain,
      chain,
      chain,
      iptablesRule.get(),
      iptablesRule.get()).get();

  if (os::system(script) != 0) {
    return ErrnoError("Failed to add DNAT rule with tag");
  }

  return Nothing();
}


Try<Nothing> PortMapper::delPortMapping()
{
  // The iptables command searches for the DNAT rules with tag
  // "container_id: <CNI_CONTAINERID>", and if it exists goes ahead
  // and deletes it.
  //
  // NOTE: We use a temp file here, instead of letting `sed` directly
  // executing the iptables commands because otherwise, it is possible
  // that the port mapping cleanup command will cause iptables to
  // deadlock if there are a lot of entires in the iptables, because
  // the `sed` won't process the next line while executing `iptables
  // -w -t nat -D ...`. But the executing of `iptables -w -t nat -D
  // ...` might get stuck if the first command `iptables -w -t nat -S
  // <TAG>` didn't finish (because the xtables lock is not released).
  // The first command might not finish if it has a lot of output,
  // filling the pipe that `sed` hasn't had a chance to process yet.
  // See details in MESOS-9127.
  string script = strings::format(
      R"~(
      #!/bin/sh
      set -x
      set -e

      FILE=$(mktemp)

      cleanup() {
        rm -f "$FILE"
      }

      trap cleanup EXIT

      iptables -w -t nat -S %s | sed -n "/%s/ s/-A/iptables -w -t nat -D/p" > $FILE
      sh $FILE
      )~",
      chain,
      getIptablesRuleTag()).get();

  // NOTE: Ideally we should be cleaning up the iptables chain in case
  // this is the last rule in the chain. However, this is a bit tricky
  // since when we try deleting the chain another incarnation of the
  // `mesos-cni-port-mapper` plugin might be injecting rules into the
  // same chain causing a race. Further, there is no way to ascertain
  // the failure due to this race, and returning a failure in this
  // scenario would (erroneously) fail the deletion of the container.
  // We therefore leave it to the operator to delete the iptables
  // chain once he deems it safe to remove the chain.
  if (os::system(script) != 0) {
    return ErrnoError("Unable to delete DNAT rules");
  }

  return Nothing();
}


Try<string, PluginError> PortMapper::handleAddCommand()
{
  Result<spec::NetworkInfo> delegateResult = delegate(cniCommand);
  if (delegateResult.isError()) {
    return PluginError(
        "Could not execute the delegate plugin '" +
        delegatePlugin + "' for ADD command: " + delegateResult.error(),
        ERROR_DELEGATE_FAILURE);
  }

  cerr << "Delegate CNI plugin '" << delegatePlugin
       << "' executed successfully for ADD command: "
       << JSON::protobuf(delegateResult.get()) << endl;

  // We support only IPv4.
  if (!delegateResult->has_ip4()) {
    return PluginError(
        "Delegate CNI plugin '" + delegatePlugin +
        "' did not return an IPv4 address",
        ERROR_DELEGATE_FAILURE);
  }

  // The IP from `delegateResult->ip4().ip()` is in CIDR notation. We
  // need to strip out the netmask.
  Try<net::IP::Network> ip = net::IP::Network::parse(
      delegateResult->ip4().ip(),
      AF_INET);

  if (ip.isError()) {
    return PluginError(
        "Could not parse IPv4 address return by delegate CNI plugin '" +
        delegatePlugin + "': " + ip.error(),
        ERROR_DELEGATE_FAILURE);
  }

  // Walk through each of the port-mappings and install a
  // D-NAT rule for the port-map.
  foreach (const NetworkInfo::PortMapping& portMapping,
           networkInfo.port_mappings()) {
    Try<Nothing> result = addPortMapping(ip->address(), portMapping);
    if (result.isError()) {
      return PluginError(result.error(), ERROR_PORTMAP_FAILURE);
    }
  }

  return stringify(JSON::protobuf(delegateResult.get()));
}


Try<Nothing, PluginError> PortMapper::handleDelCommand()
{
  // In case of `spec::CNI_CMD_DEL` we want to first delete the
  // iptables DNAT rules and then invoke the delegate plugin with the
  // DEL command. Reason being that if the delegate plugin is invoked
  // before the iptables DNAT rules are removed, the IP address
  // associated with the current container might be re-allocated to a
  // new container, which might start receiving traffic hitting these
  // DNAT rules.
  Try<Nothing> result = delPortMapping();
  if (result.isError()) {
    return PluginError(
        "Unable to remove iptables DNAT rules: " + result.error(),
        ERROR_PORTMAP_FAILURE);
  }

  cerr << "Launching delegate CNI plugin '" << delegatePlugin
       << "' with DEL command" << endl;

  Result<spec::NetworkInfo> delegateResult = delegate(spec::CNI_CMD_DEL);
  if (delegateResult.isError()) {
    return PluginError(
        "Could not execute the delegate plugin '" +
        delegatePlugin + "' for DEL command: " + delegateResult.error(),
        ERROR_DELEGATE_FAILURE);
  }

  cerr << "Successfully removed iptables DNAT rule and detached container "
       << "using CNI delegate plugin '" << delegatePlugin << "'" << endl;

  return Nothing();
}


Try<Option<string>, PluginError> PortMapper::execute()
{
  if (cniCommand == spec::CNI_CMD_ADD) {
    Try<string, PluginError> result = handleAddCommand();
    if (result.isError()) {
      return result.error();
    }

    return result.get();
  } else if (cniCommand == spec::CNI_CMD_DEL) {
    Try<Nothing, PluginError> result = handleDelCommand();
    if (result.isError()) {
      return result.error();
    }

    return None();
  }

  return PluginError(
      "Unsupported command: " + cniCommand,
      ERROR_UNSUPPORTED_COMMAND);
}


Result<spec::NetworkInfo> PortMapper::delegate(const string& command)
{
  map<string, string> environment;

  environment["CNI_COMMAND"] = command;
  environment["CNI_IFNAME"] = cniIfName;
  environment["CNI_PATH"] = cniPath;
  environment["CNI_CONTAINERID"] = cniContainerId;

  if (cniNetNs.isSome()) {
    environment["CNI_NETNS"] = cniNetNs.get();
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
    environment["PATH"] = os::host_default_path();
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
        "Failed to exec the delegate CNI plugin '" + delegatePlugin +
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

  const Future<Option<int>>& status = std::get<0>(result.get());
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
  const Future<string>& output = std::get<1>(result.get());
  if (!output.isReady()) {
    return Error(
        "Failed to read stdout from the delegate CNI plugin '" +
        delegatePlugin + "' subprocess: " +
        (output.isFailed() ? output.failure() : "discarded"));
  }

  // We are reading stderr of the plugin since any log messages from
  // the CNI plugin would be thrown on stderr. This can be useful for
  // debugging issues when the plugin throws a `spec::Error`.
  const Future<string>& err = std::get<2>(result.get());
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
        ". Could not attach/detach container: " + output.get());
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
