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

#include <mesos/module/resource_estimator.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>

using namespace mesos;
using namespace process;

using mesos::modules::Module;

using mesos::slave::ResourceEstimator;


class FixedResourceEstimatorProcess
  : public Process<FixedResourceEstimatorProcess>
{
public:
  FixedResourceEstimatorProcess(
      const lambda::function<Future<ResourceUsage>()>& _usage,
      const Resources& _totalRevocable)
    : ProcessBase(process::ID::generate("fixed-resource-estimator")),
      usage(_usage),
      totalRevocable(_totalRevocable) {}

  Future<Resources> oversubscribable()
  {
    return usage().then(defer(self(), &Self::_oversubscribable, lambda::_1));
  }

  Future<Resources> _oversubscribable(const ResourceUsage& usage)
  {
    Resources allocatedRevocable;
    foreach (const ResourceUsage::Executor& executor, usage.executors()) {
      allocatedRevocable += Resources(executor.allocated()).revocable();
    }

    auto unallocated = [](const Resources& resources) {
      Resources result = resources;
      result.unallocate();
      return result;
    };

    return totalRevocable - unallocated(allocatedRevocable);
  }

protected:
  const lambda::function<Future<ResourceUsage>()> usage;
  const Resources totalRevocable;
};


class FixedResourceEstimator : public ResourceEstimator
{
public:
  FixedResourceEstimator(const Resources& _totalRevocable)
  {
    // Mark all resources as revocable.
    foreach (Resource resource, _totalRevocable) {
      resource.mutable_revocable();
      totalRevocable += resource;
    }
  }

  ~FixedResourceEstimator() override
  {
    if (process.get() != nullptr) {
      terminate(process.get());
      wait(process.get());
    }
  }

  Try<Nothing> initialize(
      const lambda::function<Future<ResourceUsage>()>& usage) override
  {
    if (process.get() != nullptr) {
      return Error("Fixed resource estimator has already been initialized");
    }

    process.reset(new FixedResourceEstimatorProcess(usage, totalRevocable));
    spawn(process.get());

    return Nothing();
  }

  Future<Resources> oversubscribable() override
  {
    if (process.get() == nullptr) {
      return Failure("Fixed resource estimator is not initialized");
    }

    return dispatch(
        process.get(),
        &FixedResourceEstimatorProcess::oversubscribable);
  }

private:
  Resources totalRevocable;
  Owned<FixedResourceEstimatorProcess> process;
};


static bool compatible()
{
  // TODO(jieyu): Check compatibility.
  return true;
}


static ResourceEstimator* create(const Parameters& parameters)
{
  // Obtain the *fixed* resources from parameters.
  Option<Resources> resources;
  foreach (const Parameter& parameter, parameters.parameter()) {
    if (parameter.key() == "resources") {
      Try<Resources> _resources = Resources::parse(parameter.value());
      if (_resources.isError()) {
        return nullptr;
      }

      resources = _resources.get();
    }
  }

  if (resources.isNone()) {
    return nullptr;
  }

  return new FixedResourceEstimator(resources.get());
}


Module<ResourceEstimator> org_apache_mesos_FixedResourceEstimator(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Fixed Resource Estimator Module.",
    compatible,
    create);
