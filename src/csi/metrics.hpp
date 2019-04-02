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

#ifndef __CSI_METRICS_HPP__
#define __CSI_METRICS_HPP__

#include <string>

#include <process/metrics/counter.hpp>
#include <process/metrics/push_gauge.hpp>

namespace mesos {
namespace csi {

struct Metrics
{
  explicit Metrics(const std::string& prefix);

  ~Metrics();

  process::metrics::Counter csi_plugin_container_terminations;
  process::metrics::PushGauge csi_plugin_rpcs_pending;
  process::metrics::Counter csi_plugin_rpcs_finished;
  process::metrics::Counter csi_plugin_rpcs_failed;
  process::metrics::Counter csi_plugin_rpcs_cancelled;
};

} // namespace csi {
} // namespace mesos {

#endif // __CSI_METRICS_HPP__
