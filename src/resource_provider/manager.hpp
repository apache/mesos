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

#ifndef __RESOURCE_PROVIDER_MANAGER_HPP__
#define __RESOURCE_PROVIDER_MANAGER_HPP__

#include <process/authenticator.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/queue.hpp>

#include "resource_provider/message.hpp"

namespace mesos {
namespace internal {

// Forward declarations.
class ResourceProviderManagerProcess;


class ResourceProviderManager
{
public:
  ResourceProviderManager();
  ~ResourceProviderManager();

  ResourceProviderManager(
      const ResourceProviderManager& other) = delete;

  ResourceProviderManager& operator=(
      const ResourceProviderManager& other) = delete;

  // The API endpoint handler.
  process::Future<process::http::Response> api(
      const process::http::Request& request,
      const Option<process::http::authentication::Principal>& principal) const;

  // Returns a stream of messages from the resource provider manager.
  process::Queue<ResourceProviderMessage> messages() const;

private:
  process::Owned<ResourceProviderManagerProcess> process;
};

} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_MANAGER_HPP__
