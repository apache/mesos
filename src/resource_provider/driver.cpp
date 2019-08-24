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

#include <mesos/v1/resource_provider.hpp>

#include <string>
#include <utility>

#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include "internal/devolve.hpp"

#include "resource_provider/detector.hpp"
#include "resource_provider/http_connection.hpp"
#include "resource_provider/validation.hpp"

using process::Future;
using process::Owned;
using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

using std::function;
using std::string;
using std::queue;

namespace {

Option<Error> validate(const mesos::v1::resource_provider::Call& call)
{
  return mesos::internal::resource_provider::validation::call::validate(
      mesos::internal::devolve(call), None());
}

} // namespace {

namespace mesos {
namespace v1 {
namespace resource_provider {

Driver::Driver(
    Owned<mesos::internal::EndpointDetector> detector,
    ContentType contentType,
    const function<void(void)>& connected,
    const function<void(void)>& disconnected,
    const function<void(const queue<Event>&)>& received,
    const Option<string>& token)
  : process(new DriverProcess(
        "resource-provider-driver",
        std::move(detector),
        contentType,
        token,
        validate,
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


Future<Nothing> Driver::send(const Call& call)
{
  return dispatch(process.get(), &DriverProcess::send, call);
}


void Driver::start() const
{
  return dispatch(process.get(), &DriverProcess::start);
}

} // namespace resource_provider {
} // namespace v1 {
} // namespace mesos {
