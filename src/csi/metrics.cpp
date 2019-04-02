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

#include <process/metrics/metrics.hpp>

using std::string;

namespace mesos {
namespace csi {

Metrics::Metrics(const string& prefix)
  : csi_plugin_container_terminations(
        prefix + "csi_plugin/container_terminations"),
    csi_plugin_rpcs_pending(prefix + "csi_plugin/rpcs_pending"),
    csi_plugin_rpcs_finished(prefix + "csi_plugin/rpcs_finished"),
    csi_plugin_rpcs_failed(prefix + "csi_plugin/rpcs_failed"),
    csi_plugin_rpcs_cancelled(prefix + "csi_plugin/rpcs_cancelled")
{
  process::metrics::add(csi_plugin_container_terminations);
  process::metrics::add(csi_plugin_rpcs_pending);
  process::metrics::add(csi_plugin_rpcs_finished);
  process::metrics::add(csi_plugin_rpcs_failed);
  process::metrics::add(csi_plugin_rpcs_cancelled);
}


Metrics::~Metrics()
{
  process::metrics::remove(csi_plugin_container_terminations);
  process::metrics::remove(csi_plugin_rpcs_pending);
  process::metrics::remove(csi_plugin_rpcs_finished);
  process::metrics::remove(csi_plugin_rpcs_failed);
  process::metrics::remove(csi_plugin_rpcs_cancelled);
}

} // namespace csi {
} // namespace mesos {
