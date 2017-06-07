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

#include <glog/logging.h>

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <mesos/v1/resource_provider.hpp>

using std::function;
using std::queue;

using process::Owned;
using process::Process;
using process::ProcessBase;

using process::spawn;
using process::terminate;
using process::wait;

namespace mesos {
namespace v1 {
namespace resource_provider {

class DriverProcess : public Process<DriverProcess>
{
public:
  DriverProcess(
      ContentType _contentType,
      const function<void(void)>& connected,
      const function<void(void)>& disconnected,
      const function<void(const queue<Event>&)>& received)
    : ProcessBase(process::ID::generate("resource-provider-driver")),
      contentType(_contentType),
      callbacks {connected, disconnected, received} {}

protected:
  struct Callbacks
  {
    function<void(void)> connected;
    function<void(void)> disconnected;
    function<void(const queue<Event>&)> received;
  };

  const ContentType contentType;
  const Callbacks callbacks;
};


Driver::Driver(
    ContentType contentType,
    const function<void(void)>& connected,
    const function<void(void)>& disconnected,
    const function<void(const std::queue<Event>&)>& received)
  : process(new DriverProcess(
        contentType,
        connected,
        disconnected,
        received))
{
  spawn(CHECK_NOTNULL(process.get()));
}


Driver::~Driver()
{
  terminate(process.get());
  wait(process.get());
}

} // namespace resource_provider {
} // namespace v1 {
} // namespace mesos {
