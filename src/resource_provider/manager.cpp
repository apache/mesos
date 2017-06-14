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

#include "resource_provider/manager.hpp"

#include <glog/logging.h>

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

namespace http = process::http;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::ProcessBase;
using process::Queue;

using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

using process::http::authentication::Principal;

namespace mesos {
namespace internal {

class ResourceProviderManagerProcess
  : public Process<ResourceProviderManagerProcess>
{
public:
  ResourceProviderManagerProcess()
    : ProcessBase(process::ID::generate("resource-provider-manager")) {}

  Future<http::Response> api(
      const http::Request& request,
      const Option<Principal>& principal);

  Queue<ResourceProviderMessage> messages;
};


Future<http::Response> ResourceProviderManagerProcess::api(
    const http::Request& request,
    const Option<Principal>& principal)
{
  return Failure("Unimplemented");
}


ResourceProviderManager::ResourceProviderManager()
  : process(new ResourceProviderManagerProcess())
{
  spawn(CHECK_NOTNULL(process.get()));
}


ResourceProviderManager::~ResourceProviderManager()
{
  terminate(process.get());
  wait(process.get());
}


Future<http::Response> ResourceProviderManager::api(
    const http::Request& request,
    const Option<Principal>& principal) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::api,
      request,
      principal);
}


Queue<ResourceProviderMessage> ResourceProviderManager::messages() const
{
  return process->messages;
}

} // namespace internal {
} // namespace mesos {
