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

#ifndef __RESOURCE_PROVIDER_LOCAL_HPP__
#define __RESOURCE_PROVIDER_LOCAL_HPP__

#include <mesos/mesos.hpp>

#include <process/authenticator.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/error.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {

class LocalResourceProvider
{
public:
  static Try<process::Owned<LocalResourceProvider>> create(
      const process::http::URL& url,
      const std::string& workDir,
      const ResourceProviderInfo& info,
      const SlaveID& slaveId,
      const Option<std::string>& authToken,
      bool strict);

  static Option<Error> validate(const ResourceProviderInfo& info);

  // Returns a principal that contains a 'cid_prefix' claim based on the type
  // and name of the resource provider. The resource provider can only launch
  // standalone containers whose ID is prefixed by the value of 'cid_prefix'.
  // TODO(chhsiao): We should move this to a utility function.
  static process::http::authentication::Principal principal(
      const ResourceProviderInfo& info);

  virtual ~LocalResourceProvider() = default;
};

} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_LOCAL_HPP__
