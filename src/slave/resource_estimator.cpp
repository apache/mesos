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

#include <stout/error.hpp>

#include <mesos/module/resource_estimator.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include "module/manager.hpp"

#include "slave/resource_estimators/noop.hpp"

using std::string;

namespace mesos {
namespace slave {

Try<ResourceEstimator*> ResourceEstimator::create(const Option<string>& type)
{
  if (type.isNone()) {
    return new internal::slave::NoopResourceEstimator();
  }

  // Try to load resource estimator from module.
  Try<ResourceEstimator*> module =
    modules::ModuleManager::create<ResourceEstimator>(type.get());

  if (module.isError()) {
    return Error(
        "Failed to create resource estimator module '" + type.get() +
        "': " + module.error());
  }

  return module.get();
}

} // namespace slave {
} // namespace mesos {
