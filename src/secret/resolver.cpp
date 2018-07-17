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

#include <string>

#include <mesos/mesos.hpp>

#include <mesos/module/secret_resolver.hpp>

#include <mesos/secret/resolver.hpp>

#include <process/future.hpp>

#include <stout/try.hpp>

#include "module/manager.hpp"

using std::string;

using process::Failure;
using process::Future;
using process::Shared;

namespace mesos {
namespace internal {

// The default implementation verifies that the incoming secret
// contains `value` but not `reference`. It then returns the value.
class DefaultSecretResolver : public SecretResolver
{
public:
  DefaultSecretResolver() {}

  ~DefaultSecretResolver() override {}

  process::Future<Secret::Value> resolve(const Secret& secret) const override
  {
    if (secret.has_reference()) {
      return Failure("Default secret resolver cannot resolve references");
    }

    if (!secret.has_value()) {
      return Failure("Secret has no value");
    }

    return secret.value();
  }
};

} // namespace internal {


Try<SecretResolver*> SecretResolver::create(const Option<string>& moduleName)
{
  if (moduleName.isNone()) {
    LOG(INFO) << "Creating default secret resolver";
    return new internal::DefaultSecretResolver();
  }

  LOG(INFO) << "Creating secret resolver '" << moduleName.get() << "'";

  Try<SecretResolver*> result =
    modules::ModuleManager::create<SecretResolver>(moduleName.get());

  if (result.isError()) {
    return Error("Failed to initialize secret resolver: " + result.error());
  }

  return result;
}

} // namespace mesos {
