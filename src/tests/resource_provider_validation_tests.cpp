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

#include <gtest/gtest.h>

#include <mesos/mesos.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "resource_provider/validation.hpp"

namespace call = mesos::internal::resource_provider::validation::call;

using mesos::resource_provider::Call;

namespace mesos {
namespace internal {
namespace tests {

TEST(ResourceProviderCallValidationTest, Subscribe)
{
  Call call;
  call.set_type(Call::SUBSCRIBE);

  // Expecting `Call::Subscribe`.
  Option<Error> error = call::validate(call);
  EXPECT_SOME(error);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  ResourceProviderInfo* info = subscribe->mutable_resource_provider_info();
  info->set_type("org.apache.mesos.rp.test");
  info->set_name("test");

  error = call::validate(call);
  EXPECT_NONE(error);
}


TEST(ResourceProviderCallValidationTest, Update)
{
  Call call;
  call.set_type(Call::UPDATE);

  // Expecting a resource provider ID and `Call::Update`.
  Option<Error> error = call::validate(call);
  EXPECT_SOME(error);

  ResourceProviderID* id = call.mutable_resource_provider_id();
  id->set_value(UUID::random().toString());

  // Still expecting `Call::Update`.
  error = call::validate(call);
  EXPECT_SOME(error);

  Call::Update* update = call.mutable_update();
  update->set_state(Call::Update::OK);
  update->mutable_operation();

  error = call::validate(call);
  EXPECT_NONE(error);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
