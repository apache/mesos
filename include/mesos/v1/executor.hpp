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

#ifndef __MESOS_V1_EXECUTOR_HPP__
#define __MESOS_V1_EXECUTOR_HPP__

#include <functional>
#include <queue>
#include <string>

#include <mesos/http.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <process/owned.hpp>

namespace mesos {
namespace v1 {
namespace executor {

class MesosProcess; // Forward declaration.

class MesosBase
{
public:
  // Empty virtual destructor (necessary to instantiate subclasses).
  virtual ~MesosBase() {}
  virtual void send(const Call& call) = 0;
};


// Interface to Mesos for an executor.
//
// Expects three callbacks, 'connected', 'disconnected', and
// 'received' which will get invoked _serially_ when it's determined
// that we've connected (i.e. established TCP connection), disconnected
// (i.e, connection is broken), or received events from the agent.
// Note that we drop events while disconnected.
class Mesos : public MesosBase
{
public:
  Mesos(ContentType contentType,
        const std::function<void(void)>& connected,
        const std::function<void(void)>& disconnected,
        const std::function<void(const std::queue<Event>&)>& received);

  // Delete copy constructor.
  Mesos(const Mesos& other) = delete;

  // Delete assignment operator.
  Mesos& operator=(const Mesos& other) = delete;

  virtual ~Mesos();

  // Attempts to send a call to the agent.
  //
  // Some local validation of calls is performed which may result in dropped
  // events without ever being sent to the agent.
  virtual void send(const Call& call) override;

private:
  process::Owned<MesosProcess> process;
};

} // namespace executor {
} // namespace v1 {
} // namespace mesos {

#endif // __MESOS_V1_EXECUTOR_HPP__
