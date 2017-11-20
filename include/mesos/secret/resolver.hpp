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

#ifndef __MESOS_SECRET_RESOLVER_HPP__
#define __MESOS_SECRET_RESOLVER_HPP__

#include <mesos/mesos.hpp>

#include <process/future.hpp>

namespace mesos {

// This interface is used to resolve a `Secret` type into data bytes.
//
// The `resolve()` method takes a `Secret` object, potentially communicates
// with a secret-store backend, and returns `Future<Secret::Value>`. If the
// secret cannot be resolved (e.g., secret is invalid), the future fails.
//
// NOTE: The `create()` call should return a dynamically allocated object
// whose lifecycle is then delegated to the master/agent.
class SecretResolver
{
public:
  // Factory method used to create a SecretResolver instance. If the
  // `name` parameter is provided, the module is instantiated
  // using the `ModuleManager`. Otherwise, a "default" secret resolver
  // instance (defined in `src/secret/resolver.cpp`) is returned.
  static Try<SecretResolver*> create(const Option<std::string>& name = None());

  virtual ~SecretResolver() {}

  // Validates the given secret, resolves the secret reference (by potentially
  // querying a secret backend store), and returns the data associated with
  // the secret.
  virtual process::Future<Secret::Value> resolve(
      const Secret& secret) const = 0;

protected:
  SecretResolver() {}
};

} // namespace mesos {

#endif // __MESOS_SECRET_RESOLVER_HPP__
