/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <list>

#include <mesos/resources.hpp>

#include <mesos/module/resource_estimator.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include <process/dispatch.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>

using namespace mesos;
using namespace process;

using mesos::modules::Module;

using mesos::slave::ResourceEstimator;

using std::list;


class FixedResourceEstimatorProcess
  : public Process<FixedResourceEstimatorProcess>
{
public:
  FixedResourceEstimatorProcess(
      const lambda::function<Future<list<ResourceUsage>>()>& _usages,
      const Resources& _resources)
    : usages(_usages),
      resources(_resources) {}

  Future<Resources> oversubscribable()
  {
    // TODO(jieyu): This is a stub implementation.
    return resources;
  }

protected:
  const lambda::function<Future<list<ResourceUsage>>()>& usages;
  const Resources resources;
};


class FixedResourceEstimator : public ResourceEstimator
{
public:
  FixedResourceEstimator(const Resources& _resources)
    : resources(_resources)
  {
    // Mark all resources as revocable.
    foreach (Resource& resource, resources) {
      resource.mutable_revocable();
    }
  }

  virtual ~FixedResourceEstimator()
  {
    if (process.get() != NULL) {
      terminate(process.get());
      wait(process.get());
    }
  }

  virtual Try<Nothing> initialize(
      const lambda::function<Future<list<ResourceUsage>>()>& usages)
  {
    if (process.get() != NULL) {
      return Error("Fixed resource estimator has already been initialized");
    }

    process.reset(new FixedResourceEstimatorProcess(usages, resources));
    spawn(process.get());

    return Nothing();
  }

  virtual Future<Resources> oversubscribable()
  {
    if (process.get() == NULL) {
      return Failure("Fixed resource estimator is not initialized");
    }

    return dispatch(
        process.get(),
        &FixedResourceEstimatorProcess::oversubscribable);
  }

private:
  Resources resources;
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
        return NULL;
      }

      resources = _resources.get();
    }
  }

  if (resources.isNone()) {
    return NULL;
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
