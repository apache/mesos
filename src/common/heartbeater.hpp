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

#ifndef __COMMON_HEARTBEATER_HPP__
#define __COMMON_HEARTBEATER_HPP__

#include <string>

#include <process/delay.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/owned.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>

#include "common/http.hpp"

namespace mesos {
namespace internal {

// This process periodically sends heartbeats to a given HTTP streaming
// response. The optional delay parameter is used to specify the delay
// period before sending the first heartbeat. The optional callback parameter
// will be called each time a heartbeat is sent.
template<typename Message, typename Event>
class ResponseHeartbeaterProcess
  : public process::Process<ResponseHeartbeaterProcess<Message, Event>>
{
public:
  ResponseHeartbeaterProcess(
      const std::string& _logMessage,
      const Message& _heartbeatMessage,
      const StreamingHttpConnection<Event>& _connection,
      const Duration& _interval,
      const Option<Duration>& _delay = None(),
      const Option<lambda::function<void()>>& _callback = None())
    : process::ProcessBase(process::ID::generate("heartbeater")),
      logMessage(_logMessage),
      heartbeatMessage(_heartbeatMessage),
      connection(_connection),
      interval(_interval),
      delay(_delay),
      callback(_callback) {}

protected:
  void initialize() override
  {
    if (delay.isSome()) {
      process::delay(
          delay.get(),
          this,
          &ResponseHeartbeaterProcess::heartbeat);
    } else {
      heartbeat();
    }
  }

private:
  void heartbeat()
  {
    // Only send a heartbeat if the connection is not closed.
    if (connection.closed().isPending()) {
      VLOG(2) << "Sending heartbeat to " << logMessage;

      if (callback.isSome()) {
        callback.get()();
      }

      connection.send(heartbeatMessage);
    }

    process::delay(interval, this, &ResponseHeartbeaterProcess::heartbeat);
  }

  const std::string logMessage;
  const Message heartbeatMessage;
  StreamingHttpConnection<Event> connection;
  const Duration interval;
  const Option<Duration> delay;
  const Option<lambda::function<void()>> callback;
};


template<typename Message, typename Event>
class ResponseHeartbeater
{
public:
  ResponseHeartbeater(
      const std::string& _logMessage,
      const Message& _heartbeatMessage,
      const StreamingHttpConnection<Event>& _connection,
      const Duration& _interval,
      const Option<Duration>& _delay = None(),
      const Option<lambda::function<void()>>& _callback = None())
    : process(new ResponseHeartbeaterProcess<Message, Event>(
          _logMessage,
          _heartbeatMessage,
          _connection,
          _interval,
          _delay,
          _callback))
  {
    process::spawn(process.get());
  }

  ~ResponseHeartbeater()
  {
    process::terminate(process.get());
    process::wait(process.get());
  }

  // Not copyable, not assignable.
  ResponseHeartbeater(const ResponseHeartbeater&) = delete;
  ResponseHeartbeater& operator=(const ResponseHeartbeater&) = delete;

private:
  const process::Owned<ResponseHeartbeaterProcess<Message, Event>> process;
};

} // namespace internal {
} // namespace mesos {

#endif // __COMMON_HEARTBEATER_HPP__
