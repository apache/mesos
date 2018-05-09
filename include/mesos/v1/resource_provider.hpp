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

#ifndef __MESOS_V1_RESOURCE_PROVIDER_HPP__
#define __MESOS_V1_RESOURCE_PROVIDER_HPP__

#include <functional>
#include <queue>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <mesos/http.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/resource_provider/resource_provider.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>

namespace mesos {
namespace internal {

// Forward declarations.
template <typename Call, typename Event>
class HttpConnectionProcess;

class EndpointDetector;

} // namespace internal {

namespace v1 {
namespace resource_provider {

typedef ::mesos::internal::HttpConnectionProcess<Call, Event> DriverProcess;


/**
 * Resource provider driver.
 */
class Driver
{
public:
  /**
   * Construct a Driver.
   *
   * Expects three callbacks, `connected`, `disconnected`, and
   * `received` which will get invoked _serially_ when it's determined
   * that we've connected (i.e. established TCP connection),
   * disconnected (i.e, connection is broken), or received events from
   * the resource provider manager. Note that we drop events while
   * disconnected.
   *
   * @param url the URL where the resource provider API is served.
   * @param contentType the content type expected by this driver.
   * @param connected a callback which will be invoked when the driver
   *     is connected.
   * @param disconnected a callback which will be invoked when the
   *     driver is disconnected.
   * @param received a callback which will be invoked when the driver
   *     receives resource provider Events.
   * @param token the token that will be used for authentication.
   */
  Driver(process::Owned<mesos::internal::EndpointDetector> detector,
         ContentType contentType,
         const std::function<void(void)>& connected,
         const std::function<void(void)>& disconnected,
         const std::function<void(const std::queue<Event>&)>& received,
         const Option<std::string>& token);

  ~Driver();

  void start() const;

  Driver(const Driver& other) = delete;
  Driver& operator=(const Driver& other) = delete;

  process::Future<Nothing> send(const Call& call);

private:
  process::Owned<DriverProcess> process;
};

} // namespace resource_provider {
} // namespace v1 {
} // namespace mesos {

#endif // __MESOS_V1_RESOURCE_PROVIDER_HPP__
