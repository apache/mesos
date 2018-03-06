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

#include <mesos/module/qos_controller.hpp>

#include <mesos/slave/qos_controller.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>

#include "slave/qos_controllers/load.hpp"

using namespace mesos;
using namespace process;

using std::list;

using mesos::modules::Module;

using mesos::slave::QoSController;
using mesos::slave::QoSCorrection;

namespace mesos {
namespace internal {
namespace slave {


class LoadQoSControllerProcess : public Process<LoadQoSControllerProcess>
{
public:
  LoadQoSControllerProcess(
      const lambda::function<Future<ResourceUsage>()>& _usage,
      const lambda::function<Try<os::Load>()>& _loadAverage,
      const Option<double>& _loadThreshold5Min,
      const Option<double>& _loadThreshold15Min)
    : ProcessBase(process::ID::generate("qos-load-controller")),
      usage(_usage),
      loadAverage(_loadAverage),
      loadThreshold5Min(_loadThreshold5Min),
      loadThreshold15Min(_loadThreshold15Min) {}

  Future<list<QoSCorrection>> corrections()
  {
    return usage().then(defer(self(), &Self::_corrections, lambda::_1));
  }

  Future<list<QoSCorrection>> _corrections(const ResourceUsage& usage)
  {
    Try<os::Load> load = loadAverage();
    if (load.isError()) {
      LOG(ERROR) << "Failed to fetch system load: " + load.error();
      return list<QoSCorrection>();
    }

    bool overloaded = false;

    if (loadThreshold5Min.isSome()) {
      if (load->five > loadThreshold5Min.get()) {
        LOG(INFO) << "System 5 minutes load average " << load->five
                  << " exceeds threshold " << loadThreshold5Min.get();
        overloaded = true;
      }
    }

    if (loadThreshold15Min.isSome()) {
      if (load->fifteen > loadThreshold15Min.get()) {
        LOG(INFO) << "System 15 minutes load average " << load->fifteen
                  << " exceeds threshold " << loadThreshold15Min.get();
        overloaded = true;
      }
    }

    if (overloaded) {
      list<QoSCorrection> corrections;

      foreach (const ResourceUsage::Executor& executor, usage.executors()) {
        // Set kill correction for all revocable executors.
        if (!Resources(executor.allocated()).revocable().empty()) {
          QoSCorrection correction;

          correction.set_type(mesos::slave::QoSCorrection_Type_KILL);
          correction.mutable_kill()->mutable_framework_id()->CopyFrom(
            executor.executor_info().framework_id());
          correction.mutable_kill()->mutable_executor_id()->CopyFrom(
            executor.executor_info().executor_id());

          corrections.push_back(correction);
        }
      }

      return corrections;
    }

    return list<QoSCorrection>();
  }

private:
  const lambda::function<Future<ResourceUsage>()> usage;
  const lambda::function<Try<os::Load>()> loadAverage;
  const Option<double> loadThreshold5Min;
  const Option<double> loadThreshold15Min;
};


LoadQoSController::~LoadQoSController()
{
  if (process.get() != nullptr) {
    terminate(process.get());
    wait(process.get());
  }
}


Try<Nothing> LoadQoSController::initialize(
  const lambda::function<Future<ResourceUsage>()>& usage)
{
  if (process.get() != nullptr) {
    return Error("Load QoS Controller has already been initialized");
  }

  process.reset(
      new LoadQoSControllerProcess(
          usage,
          loadAverage,
          loadThreshold5Min,
          loadThreshold15Min));

  spawn(process.get());

  return Nothing();
}


process::Future<list<QoSCorrection>> LoadQoSController::corrections()
{
  if (process.get() == nullptr) {
    return Failure("Load QoS Controller is not initialized");
  }

  return dispatch(
      process.get(),
      &LoadQoSControllerProcess::corrections);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {


static QoSController* create(const Parameters& parameters)
{
  // Obtain the system load threshold from parameters.
  Option<double> loadThreshold5Min = None();
  Option<double> loadThreshold15Min = None();

  foreach (const Parameter& parameter, parameters.parameter()) {
    if (parameter.key() == "load_threshold_5min") {
      // Try to parse the load 5min value.
      Try<double> thresholdParam = numify<double>(parameter.value());
      if (thresholdParam.isError()) {
        LOG(ERROR) << "Failed to parse 5 min load threshold: "
                   << thresholdParam.error();
        return nullptr;
      }

      loadThreshold5Min = thresholdParam.get();
    } else if (parameter.key() == "load_threshold_15min") {
      // Try to parse the load 15min value.
      Try<double> thresholdParam = numify<double>(parameter.value());
      if (thresholdParam.isError()) {
        LOG(ERROR) << "Failed to parse 15 min load threshold: "
                   << thresholdParam.error();
        return nullptr;
      }

      loadThreshold15Min = thresholdParam.get();
    }
  }

  if (loadThreshold5Min.isNone() && loadThreshold15Min.isNone()) {
    LOG(ERROR) << "No load thresholds are configured for LoadQoSController";
    return nullptr;
  }

  return new mesos::internal::slave::LoadQoSController(
      loadThreshold5Min, loadThreshold15Min);
}


Module<QoSController> org_apache_mesos_LoadQoSController(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "System Load QoS Controller Module.",
    nullptr,
    create);
