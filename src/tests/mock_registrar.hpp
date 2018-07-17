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

#ifndef __TESTS_MOCK_REGISTRAR_HPP__
#define __TESTS_MOCK_REGISTRAR_HPP__

#include <string>

#include <gmock/gmock.h>

#include <mesos/state/state.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/none.hpp>
#include <stout/option.hpp>

#include "master/flags.hpp"
#include "master/registrar.hpp"

namespace mesos {
namespace internal {
namespace tests {

class MockRegistrar : public mesos::internal::master::Registrar
{
public:
  MockRegistrar(const master::Flags& flags,
                mesos::state::State* state,
                const Option<std::string>& authenticationRealm = None());

  ~MockRegistrar() override;

  MOCK_METHOD1(
      apply,
      process::Future<bool>(
          process::Owned<master::RegistryOperation> operation));

  process::Future<bool> unmocked_apply(
      process::Owned<master::RegistryOperation> operation);
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MOCK_REGISTRAR_HPP__
