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

#include <string>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/resource_estimator.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include <stout/try.hpp>

#include "slave/resource_estimators/noop.hpp"

using namespace mesos;

using mesos::internal::slave::NoopResourceEstimator;

using mesos::slave::ResourceEstimator;

static ResourceEstimator* createResourceEstimator(const Parameters& parameters)
{
  Try<ResourceEstimator*> result =
    NoopResourceEstimator::create(None());
  if (result.isError()) {
    return nullptr;
  }
  return result.get();
}


// Declares a TestResourceEstimator module named
// 'org_apache_mesos_TestNoopResourceEstimator'.
mesos::modules::Module<ResourceEstimator>
  org_apache_mesos_TestNoopResourceEstimator(
      MESOS_MODULE_API_VERSION,
      MESOS_VERSION,
      "Apache Mesos",
      "modules@mesos.apache.org",
      "Test Noop Resource Estimator module.",
      nullptr,
      createResourceEstimator);
