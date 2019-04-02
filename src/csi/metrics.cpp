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

#include "csi/metrics.hpp"

#include <vector>

#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>

using std::string;
using std::vector;

using process::metrics::Counter;
using process::metrics::PushGauge;

namespace mesos {
namespace csi {

Metrics::Metrics(const string& prefix)
  : csi_plugin_container_terminations(
        prefix + "csi_plugin/container_terminations")
{
  process::metrics::add(csi_plugin_container_terminations);

  vector<csi::v0::RPC> rpcs;

  // NOTE: We use a switch statement here as a compile-time sanity check so we
  // won't forget to add metrics for new RPCs in the future. Since each case
  // falls through intentionally, every RPC will be added.
  csi::v0::RPC firstRpc = csi::v0::GET_PLUGIN_INFO;
  switch (firstRpc) {
    case csi::v0::GET_PLUGIN_INFO:
      rpcs.push_back(csi::v0::GET_PLUGIN_INFO);
    case csi::v0::GET_PLUGIN_CAPABILITIES:
      rpcs.push_back(csi::v0::GET_PLUGIN_CAPABILITIES);
    case csi::v0::PROBE:
      rpcs.push_back(csi::v0::PROBE);
    case csi::v0::CREATE_VOLUME:
      rpcs.push_back(csi::v0::CREATE_VOLUME);
    case csi::v0::DELETE_VOLUME:
      rpcs.push_back(csi::v0::DELETE_VOLUME);
    case csi::v0::CONTROLLER_PUBLISH_VOLUME:
      rpcs.push_back(csi::v0::CONTROLLER_PUBLISH_VOLUME);
    case csi::v0::CONTROLLER_UNPUBLISH_VOLUME:
      rpcs.push_back(csi::v0::CONTROLLER_UNPUBLISH_VOLUME);
    case csi::v0::VALIDATE_VOLUME_CAPABILITIES:
      rpcs.push_back(csi::v0::VALIDATE_VOLUME_CAPABILITIES);
    case csi::v0::LIST_VOLUMES:
      rpcs.push_back(csi::v0::LIST_VOLUMES);
    case csi::v0::GET_CAPACITY:
      rpcs.push_back(csi::v0::GET_CAPACITY);
    case csi::v0::CONTROLLER_GET_CAPABILITIES:
      rpcs.push_back(csi::v0::CONTROLLER_GET_CAPABILITIES);
    case csi::v0::NODE_STAGE_VOLUME:
      rpcs.push_back(csi::v0::NODE_STAGE_VOLUME);
    case csi::v0::NODE_UNSTAGE_VOLUME:
      rpcs.push_back(csi::v0::NODE_UNSTAGE_VOLUME);
    case csi::v0::NODE_PUBLISH_VOLUME:
      rpcs.push_back(csi::v0::NODE_PUBLISH_VOLUME);
    case csi::v0::NODE_UNPUBLISH_VOLUME:
      rpcs.push_back(csi::v0::NODE_UNPUBLISH_VOLUME);
    case csi::v0::NODE_GET_ID:
      rpcs.push_back(csi::v0::NODE_GET_ID);
    case csi::v0::NODE_GET_CAPABILITIES:
      rpcs.push_back(csi::v0::NODE_GET_CAPABILITIES);
  }

  foreach (const csi::v0::RPC& rpc, rpcs) {
    const string name = stringify(rpc);

    csi_plugin_rpcs_pending.put(
        rpc, PushGauge(prefix + "csi_plugin/rpcs/" + name + "/pending"));
    csi_plugin_rpcs_successes.put(
        rpc, Counter(prefix + "csi_plugin/rpcs/" + name + "/successes"));
    csi_plugin_rpcs_errors.put(
        rpc, Counter(prefix + "csi_plugin/rpcs/" + name + "/errors"));
    csi_plugin_rpcs_cancelled.put(
        rpc, Counter(prefix + "csi_plugin/rpcs/" + name + "/cancelled"));

    process::metrics::add(csi_plugin_rpcs_pending.at(rpc));
    process::metrics::add(csi_plugin_rpcs_successes.at(rpc));
    process::metrics::add(csi_plugin_rpcs_errors.at(rpc));
    process::metrics::add(csi_plugin_rpcs_cancelled.at(rpc));
  }
}


Metrics::~Metrics()
{
  process::metrics::remove(csi_plugin_container_terminations);

  foreachvalue (const PushGauge& gauge, csi_plugin_rpcs_pending) {
    process::metrics::remove(gauge);
  }

  foreachvalue (const Counter& counter, csi_plugin_rpcs_successes) {
    process::metrics::remove(counter);
  }

  foreachvalue (const Counter& counter, csi_plugin_rpcs_errors) {
    process::metrics::remove(counter);
  }

  foreachvalue (const Counter& counter, csi_plugin_rpcs_cancelled) {
    process::metrics::remove(counter);
  }
}

} // namespace csi {
} // namespace mesos {
