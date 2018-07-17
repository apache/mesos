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
#include <memory>
#include <queue>
#include <string>

#include <mesos/http.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <process/future.hpp>

#include <stout/option.hpp>

namespace mesos {

namespace master {
namespace detector {
class MasterDetector;
} // namespace detector {
} // namespace master {

namespace v1 {
namespace scheduler {

class MesosProcess; // Forward declaration.

// Abstract interface for connecting a scheduler to Mesos.
class MesosBase
{
public:
  // Empty virtual destructor (necessary to instantiate subclasses).
  virtual ~MesosBase() {}
  virtual void send(const Call& call) = 0;
  virtual process::Future<APIResult> call(const Call& callMessage) = 0;
  virtual void reconnect() = 0;
};


// Concrete implementation that connects a scheduler to a Mesos master.
// Abstracts master detection (connection and disconnection).
//
// Expects three callbacks, 'connected', 'disconnected', and
// 'received' which will get invoked _serially_ when it's determined
// that we've connected (i.e., detected master), disconnected
// (i.e, detected no master), or received events from the master.
// The library reconnects with the master upon a disconnection.
//
// NOTE: All calls and events are dropped while disconnected.
class Mesos : public MesosBase
{
public:
  // The credential will be used for authenticating with the master. Currently,
  // only HTTP basic authentication is supported.
  Mesos(const std::string& master,
        ContentType contentType,
        const std::function<void()>& connected,
        const std::function<void()>& disconnected,
        const std::function<void(const std::queue<Event>&)>& received,
        const Option<Credential>& credential);

  // Delete copy constructor.
  Mesos(const Mesos& other) = delete;

  // Delete assignment operator.
  Mesos& operator=(const Mesos& other) = delete;

  ~Mesos() override;

  // Attempts to send a call to the master.
  //
  // The scheduler should only invoke this method once it has received the
  // 'connected' callback. Otherwise, all calls would be dropped while
  // disconnected.
  //
  // Some local validation of calls is performed which may generate
  // events without ever being sent to the master. This includes when
  // calls are sent but no master is currently detected (i.e., we're
  // disconnected).
  void send(const Call& call) override;

  // Attempts to send a call to the master, returning the response.
  //
  // The scheduler should only invoke this method once it has received the
  // 'connected' callback. Otherwise, a `Failure` will be returned.
  //
  // Some local validation of calls is performed, and the request will not be
  // sent to the master if the validation fails.
  //
  // A `Failure` will be returned on validation failures or if an error happens
  // when sending the request to the master, e.g., a master disconnection, or a
  // deserialization error.
  //
  // If it was possible to receive a response from the server, the returned
  // object will contain the HTTP response status code.
  //
  // There are three cases to consider depending on the HTTP response status
  // code:
  //
  //  (1) '202 ACCEPTED': Indicates the call was accepted for processing and
  //      neither `APIResult::response` nor `APIResult::error` will be set.
  //
  //  (2) '200 OK': Indicates the call completed successfully.
  //      `APIResult::response` will be set if the `scheduler::Call::Type`
  //      has a corresponding `scheduler::Response::Type`, `APIResult::error`
  //      will not be set.
  //
  //  (3) For all other HTTP status codes, the `APIResult::response` field will
  //      not be set and the `APIResult::error` field may be set to provide more
  //      information.
  //
  // Note: This method cannot be used to send `SUBSCRIBE` calls, use `send()`
  // instead.
  process::Future<APIResult> call(const Call& callMessage) override;

  // Force a reconnection with the master.
  //
  // In the case of a one-way network partition, the connection between the
  // scheduler and master might not necessarily break. If the scheduler detects
  // a partition, due to lack of `HEARTBEAT` events (e.g., 5) within a time
  // window, it can explicitly ask the library to force a reconnection with
  // the master.
  //
  // This call would be ignored if the scheduler is already disconnected with
  // the master (e.g., no new master has been elected). Otherwise, the scheduler
  // would get a 'disconnected' callback followed by a 'connected' callback.
  void reconnect() override;

protected:
  // NOTE: This constructor is used for testing.
  Mesos(
      const std::string& master,
      ContentType contentType,
      const std::function<void()>& connected,
      const std::function<void()>& disconnected,
      const std::function<void(const std::queue<Event>&)>& received,
      const Option<Credential>& credential,
      const Option<std::shared_ptr<mesos::master::detector::MasterDetector>>&
        detector);

  // Stops the library so that:
  //   - No more calls can be sent to the master.
  //   - No more callbacks can be made to the scheduler. In some cases, there
  //     may be one additional callback if the library was in the middle of
  //     processing an event.
  //
  // NOTE: This is used for testing.
  virtual void stop();

private:
  MesosProcess* process;
};

} // namespace scheduler {
} // namespace v1 {
} // namespace mesos {

#endif // __MESOS_V1_SCHEDULER_HPP__
