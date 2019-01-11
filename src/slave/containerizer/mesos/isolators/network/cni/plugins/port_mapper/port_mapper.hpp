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

#ifndef __NETWORK_CNI_PLUGIN_PORTMAPPER_HPP__
#define __NETWORK_CNI_PLUGIN_PORTMAPPER_HPP__

#include <map>
#include <string>

#include <stout/json.hpp>
#include <stout/option.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <mesos/mesos.hpp>

#include "slave/containerizer/mesos/isolators/network/cni/spec.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace cni {

class PortMapper
{
public:
  // Error codes for the port mapper plugin.
  // NOTE: Plugin specific erros should use Values of 100+.
  static constexpr int ERROR_READ_FAILURE = 100; // Fail to read from stdin.
  static constexpr int ERROR_BAD_ARGS = 101;     // Miss or invalid arguments.
  static constexpr int ERROR_DELEGATE_FAILURE = 102;
  static constexpr int ERROR_PORTMAP_FAILURE = 103;
  static constexpr int ERROR_UNSUPPORTED_COMMAND = 104;

  // Takes in a JSON formatted string, validates that the following
  // fields are present:
  //  * 'name'    : Name of the CNI configuration that will be passed
  //                to the CNI plugin used for delegation.
  //  * 'chain'   : The iptables chain to use for setting up DNAT.
  //  * 'delegate': The port-mapper should always be used in
  //                conjunction with another CNI plugin that sets up
  //                the interfaces and IP address on the container.
  //                This field holds the CNI configuration of the
  //                plugin to which the port-mapper delegates the
  //                functionality of connecting the container to the
  //                network.
  //  * 'args'    : The port-mapper uses the `NetworkInfo` passed by
  //                Mesos as meta-data in the "args" field to decipher
  //                the ports for which DNAT needs to bet setup. To
  //                understand how Mesos uses this field please read
  //                the documentation for "CNI support in Mesos":
  //                http://mesos.apache.org/documentation/latest/cni/
  //
  // In case of an error returns a JSON formatted string of type
  // `spec::Error` as the error message for the `Try`.
  static Try<process::Owned<PortMapper>, spec::PluginError> create(
      const std::string& cniConfig);

  // Executes the CNI plugin specified in 'delegate'. When
  // `cniCommand` is set to `spec::CNI_CMD_ADD` successful execution
  // of the 'delegate' plugin will install port-forwarding rules, if
  // any, that are specified in `NetworkInfo`. On success will return
  // a JSON string seen in the successful execution of the 'delegate'
  // plugin. When `cniCommand` is set to `spec::CNI_CMD_DEL`
  // successful execution of the delegate plugin will return `None`.
  Try<Option<std::string>, spec::PluginError> execute();

  virtual ~PortMapper() {};

protected:
  // Used to invoke the plugin specified in 'delegate'. The possible
  // values for `command` are `spec::CNI_CMD_ADD` or
  // `spec::CNI_CMD_DEL`. The `command` is used in
  // setting the CNI_COMMAND environment variable before invoking the
  // 'delegate' plugin.
  //
  // When `command` is set to `spec::CNI_CMD_ADD` returns a
  // `spec::NetworkInfo` on successful execution of the 'delegate'
  // plugin. When command is set to `spec::CNI_CMD_DEL` returns `None`
  // on successful execution of the plugin.
  //
  // NOTE: Defining `delegate` as a virtual method so that we can mock it.
  virtual Result<spec::NetworkInfo> delegate(const std::string& command);

private:
  PortMapper(
      const std::string& _cniCommand,       // ADD, DEL or VERSION.
      const std::string& _cniContainerId,   // Container ID.
      const Option<std::string>& _cniNetNs, // Path to network namespace file.
      const std::string& _cniIfName,        // Interface name to set up.
      const Option<std::string>& _cniArgs,  // Extra arguments.
      const std::string& _cniPath,          // Paths to search for CNI plugins.
      const mesos::NetworkInfo& _networkInfo,
      const std::string& _delegatePlugin,
      const JSON::Object& _delegateConfig,
      const std::string& _chain,
      const std::vector<std::string>& _excludeDevices)
    : cniCommand(_cniCommand),
      cniContainerId(_cniContainerId),
      cniNetNs(_cniNetNs),
      cniIfName(_cniIfName),
      cniArgs(_cniArgs),
      cniPath(_cniPath),
      networkInfo(_networkInfo),
      delegatePlugin(_delegatePlugin),
      delegateConfig(_delegateConfig),
      chain(_chain),
      excludeDevices(_excludeDevices){};

  // Returns a tag that will be appended to every DNAT rule in the
  // iptables associated with this container. Currently the tag is of
  // the form: 'container_id: <CNI_CONTAINERID>'.
  std::string getIptablesRuleTag();

  std::string getIptablesRule(
      const net::IP& ip,
      const mesos::NetworkInfo::PortMapping& portMapping);

  Try<Nothing> addPortMapping(
      const net::IP& ip,
      const mesos::NetworkInfo::PortMapping& portMapping);

  Try<Nothing> delPortMapping();

  Try<std::string, spec::PluginError> handleAddCommand();
  Try<Nothing, spec::PluginError> handleDelCommand();

  const std::string cniCommand;
  const std::string cniContainerId;
  const Option<std::string> cniNetNs;
  const std::string cniIfName;
  const Option<std::string> cniArgs;
  const std::string cniPath;

  const mesos::NetworkInfo networkInfo;

  const std::string delegatePlugin;
  const JSON::Object delegateConfig;

  // The iptable chain to which the DNAT rules need to be added. We
  // need a separate chain, so that we can group the DNAT rules
  // specific to this CNI network under this chain. It makes it easier
  // for the operator to analyze the ownership of these rules if they
  // are grouped under a chain that the operator is aware is used by
  // the CNI plugin.
  const std::string chain;

  // List of ingress devices that should be excluded from the DNAT
  // rules.
  const std::vector<std::string> excludeDevices;
};

} // namespace cni {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __NETWORK_CNI_PLUGIN_PORTMAPPER_HPP__
