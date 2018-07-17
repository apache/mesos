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

#ifndef __SLAVE_QOS_CONTROLLERS_NOOP_HPP__
#define __SLAVE_QOS_CONTROLLERS_NOOP_HPP__

#include <mesos/slave/qos_controller.hpp>

#include <stout/lambda.hpp>

#include <process/owned.hpp>

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class NoopQoSControllerProcess;


// The NOOP QoS Controller is an empty stub, which returns a future
// which is never satisfied. Thus, the slave will never carry out any
// corrections.
class NoopQoSController : public mesos::slave::QoSController
{
public:
  ~NoopQoSController() override;

  Try<Nothing> initialize(
      const lambda::function<process::Future<ResourceUsage>()>& usage) override;

  process::Future<std::list<mesos::slave::QoSCorrection>> corrections()
    override;

protected:
  process::Owned<NoopQoSControllerProcess> process;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_QOS_CONTROLLERS_NOOP_HPP__
