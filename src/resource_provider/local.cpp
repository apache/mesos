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

#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>

#include "resource_provider/local.hpp"

#include "resource_provider/storage/provider.hpp"

namespace http = process::http;

using std::string;

using process::Owned;

using process::http::authentication::Principal;

namespace mesos {
namespace internal {

Try<Owned<LocalResourceProvider>> LocalResourceProvider::create(
    const http::URL& url,
    const string& workDir,
    const ResourceProviderInfo& info,
    const SlaveID& slaveId,
    const Option<string>& authToken,
    bool strict)
{
  // TODO(jieyu): Document the built-in local resource providers.
  const hashmap<string, lambda::function<decltype(create)>> creators = {
#if defined(ENABLE_GRPC) && defined(__linux__)
    {"org.apache.mesos.rp.local.storage", &StorageLocalResourceProvider::create}
#endif
  };

  if (creators.contains(info.type())) {
    return creators.at(info.type())(
        url, workDir, info, slaveId, authToken, strict);
  }

  return Error("Unknown local resource provider type '" + info.type() + "'");
}


Try<Principal> LocalResourceProvider::principal(
    const ResourceProviderInfo& info)
{
  // TODO(chhsiao): Document the principals for built-in local resource
  // providers.
  const hashmap<string, lambda::function<decltype(principal)>>
    principalGenerators = {
#if defined(ENABLE_GRPC) && defined(__linux__)
      {"org.apache.mesos.rp.local.storage",
        &StorageLocalResourceProvider::principal}
#endif
    };

  if (principalGenerators.contains(info.type())) {
    return principalGenerators.at(info.type())(info);
  }

  return Error("Unknown local resource provider type '" + info.type() + "'");
}

} // namespace internal {
} // namespace mesos {
