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

#include <gmock/gmock.h>

#include <mesos/state/state.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/none.hpp>
#include <stout/option.hpp>

#include "master/flags.hpp"
#include "master/registrar.hpp"

#include "tests/mock_registrar.hpp"

using std::string;

using testing::_;
using testing::DoDefault;
using testing::Invoke;

using process::Future;
using process::Owned;

namespace mesos {
namespace internal {
namespace tests {

MockRegistrar::MockRegistrar(
    const master::Flags& flags,
    mesos::state::State* state,
    const Option<string>& authenticationRealm)
  : Registrar(flags, state, authenticationRealm)
{
  // The default behavior is to call the original method.
  ON_CALL(*this, apply(_))
    .WillByDefault(Invoke(this, &MockRegistrar::unmocked_apply));
  EXPECT_CALL(*this, apply(_))
    .WillRepeatedly(DoDefault());
}


MockRegistrar::~MockRegistrar() {}


Future<bool> MockRegistrar::unmocked_apply(
    Owned<master::RegistryOperation> operation)
{
  return master::Registrar::apply(operation);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
