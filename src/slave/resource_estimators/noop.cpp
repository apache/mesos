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

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>

#include "slave/resource_estimators/noop.hpp"

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

class NoopResourceEstimatorProcess :
  public Process<NoopResourceEstimatorProcess>
{
public:
  NoopResourceEstimatorProcess()
    : ProcessBase(process::ID::generate("noop-resource-estimator")) {}

  Future<Resources> oversubscribable()
  {
    return Future<Resources>();
  }
};


NoopResourceEstimator::~NoopResourceEstimator()
{
  if (process.get() != nullptr) {
    terminate(process.get());
    wait(process.get());
  }
}


Try<Nothing> NoopResourceEstimator::initialize(
    const lambda::function<Future<ResourceUsage>()>& usage)
{
  if (process.get() != nullptr) {
    return Error("Noop resource estimator has already been initialized");
  }

  process.reset(new NoopResourceEstimatorProcess());
  spawn(process.get());

  return Nothing();
}


Future<Resources> NoopResourceEstimator::oversubscribable()
{
  if (process.get() == nullptr) {
    return Failure("Noop resource estimator is not initialized");
  }

  return dispatch(
      process.get(),
      &NoopResourceEstimatorProcess::oversubscribable);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
