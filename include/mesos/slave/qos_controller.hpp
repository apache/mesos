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

#ifndef __MESOS_SLAVE_QOS_CONTROLLER_HPP__
#define __MESOS_SLAVE_QOS_CONTROLLER_HPP__

#include <list>
#include <string>

#include <mesos/resources.hpp>

#include <mesos/slave/oversubscription.hpp>

#include <process/future.hpp>
#include <process/queue.hpp>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace slave {

// A slave component used for oversubscription. When the revocable
// tasks are running, it is important to constantly monitor the
// original tasks running on those resources and guarantee performance
// based on an SLA. In order to react to detected interference, the
// QoS controller needs to be able to kill or throttle running
// revocable tasks.
class QoSController
{
public:
  // Create a QoS Controller instance of the given type specified
  // by the user. If the type is not specified, a default resource
  // estimator instance will be created.
  static Try<QoSController*> create(const Option<std::string>& type);

  virtual ~QoSController() {}

  // Initializes this QoS Controller. This method needs to be
  // called before any other member method is called. It registers
  // a callback in the QoS Controller. The callback allows the
  // QoS Controller to fetch the current resource usage for each
  // executor on slave.
  virtual Try<Nothing> initialize(
      const lambda::function<process::Future<ResourceUsage>()>& usage) = 0;

  // A QoS Controller informs the slave about corrections to carry
  // out, but returning futures to QoSCorrection objects. For more
  // information, please refer to mesos.proto.
  virtual process::Future<std::list<QoSCorrection>> corrections() = 0;
};

} // namespace slave {
} // namespace mesos {

#endif // __MESOS_SLAVE_QOS_CONTROLLER_HPP__
