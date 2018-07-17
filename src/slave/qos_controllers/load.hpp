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

#ifndef __SLAVE_QOS_CONTROLLERS_LOAD_HPP__
#define __SLAVE_QOS_CONTROLLERS_LOAD_HPP__

#include <list>

#include <mesos/slave/qos_controller.hpp>

#include <stout/lambda.hpp>
#include <stout/os/os.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class LoadQoSControllerProcess;


// The `LoadQoSController` is a simple QoS Controller, which is
// responsible for eviction of all the revocable executors when
// system load (5min or 15min) is above the configured threshold.
// NOTE: 1 minute system load is ignored, because
// for most use cases it is a misleading signal.
class LoadQoSController : public mesos::slave::QoSController
{
public:
  // NOTE: In constructor we can pass lambda for fetching load average as
  // an optional argument. This was done for the test purposes.
  LoadQoSController(
      const Option<double>& _loadThreshold5Min,
      const Option<double>& _loadThreshold15Min,
      const lambda::function<Try<os::Load>()>& _loadAverage =
        [](){ return os::loadavg(); })
    : loadThreshold5Min(_loadThreshold5Min),
      loadThreshold15Min(_loadThreshold15Min),
      loadAverage(_loadAverage) {}

  ~LoadQoSController() override;

  Try<Nothing> initialize(
    const lambda::function<process::Future<ResourceUsage>()>& usage) override;

  process::Future<std::list<mesos::slave::QoSCorrection>> corrections()
    override;

private:
  const Option<double> loadThreshold5Min;
  const Option<double> loadThreshold15Min;
  const lambda::function<Try<os::Load>()> loadAverage;
  process::Owned<LoadQoSControllerProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_QOS_CONTROLLERS_LOAD_HPP__
