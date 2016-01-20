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

#ifndef __MESOS_V1_SCHEDULER_HPP__
#define __MESOS_V1_SCHEDULER_HPP__

#include <functional>
#include <queue>
#include <string>

#include <mesos/http.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

namespace mesos {
namespace v1 {
namespace scheduler {

class MesosProcess; // Forward declaration.

// Interface to Mesos for a scheduler. Abstracts master detection
// (connection and disconnection).
//
// Expects three callbacks, 'connected', 'disconnected', and
// 'received' which will get invoked _serially_ when it's determined
// that we've connected (i.e., detected master), disconnected
// (i.e, detected no master), or received events from the master.
// Note that we drop events while disconnected.
class Mesos
{
public:
  Mesos(const std::string& master,
        ContentType contentType,
        const std::function<void()>& connected,
        const std::function<void()>& disconnected,
        const std::function<void(const std::queue<Event>&)>& received);

  // Delete copy constructor.
  Mesos(const Mesos& other) = delete;

  // Delete assignment operator.
  Mesos& operator=(const Mesos& other) = delete;

  virtual ~Mesos();

  // Attempts to send a call to the master.
  //
  // Some local validation of calls is performed which may generate
  // events without ever being sent to the master. This includes when
  // calls are sent but no master is currently detected (i.e., we're
  // disconnected).
  virtual void send(const Call& call);

private:
  MesosProcess* process;
};

} // namespace scheduler {
} // namespace v1 {
} // namespace mesos {

#endif // __MESOS_V1_SCHEDULER_HPP__
