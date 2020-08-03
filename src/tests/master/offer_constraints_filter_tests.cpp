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

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <mesos/allocator/allocator.hpp>
#include <mesos/attributes.hpp>

using std::string;
using std::unique_ptr;

using ::mesos::Attributes;
using ::mesos::SlaveInfo;

using ::mesos::allocator::OfferConstraintsFilter;

using ::mesos::scheduler::OfferConstraints;


static Try<OfferConstraints> OfferConstraintsFromJSON(const string& json)
{
  Try<JSON::Object> jsonObject = JSON::parse<JSON::Object>(json);

  if (jsonObject.isError()) {
    return Error(jsonObject.error());
  }

  return protobuf::parse<OfferConstraints>(*jsonObject);
}


static SlaveInfo slaveInfoWithAttributes(const string& attributes)
{
  SlaveInfo info;
  *info.mutable_attributes() = Attributes::parse(attributes);
  return info;
}


// Tests a single Exists constraint on a named attribute.
TEST(OfferConstraintsFilter, NamedAttributeExists)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"exists": {}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter =
    OfferConstraintsFilter::create(std::move(*constraints));

  ASSERT_SOME(filter);

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:123")));

  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("foo:123")));

  EXPECT_FALSE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("foo:123;bar:456")));
}


// Tests a single NotExists constraint on a named attribute.
TEST(OfferConstraintsFilter, NamedAttributeNotExists)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"not_exists": {}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter =
    OfferConstraintsFilter::create(std::move(*constraints));

  ASSERT_SOME(filter);

  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:123")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("foo:123")));

  EXPECT_TRUE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("foo:123;bar:456")));
}


// Tests a single group of two constraints.
TEST(OfferConstraintsFilter, TwoConstraintsInGroup)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [
              {
                "selector": {"attribute_name": "foo"},
                "predicate": {"exists": {}}
              },
              {
                "selector": {"attribute_name": "bar"},
                "predicate": {"not_exists": {}}
              }
            ]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter =
    OfferConstraintsFilter::create(std::move(*constraints));

  ASSERT_SOME(filter);

  EXPECT_TRUE(filter->isAgentExcluded("roleA", slaveInfoWithAttributes("")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("foo:123")));

  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:456")));

  EXPECT_TRUE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("bar:123;foo:456")));

  EXPECT_FALSE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("baz:123;foo:456")));
}


// Tests a constraint expression consisting of two groups.
TEST(OfferConstraintsFilter, TwoGroups)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [
            {
              "attribute_constraints": [{
                "selector": {"attribute_name": "foo"},
                "predicate": {"exists": {}}
              }]
            },
            {
              "attribute_constraints": [{
                "selector": {"attribute_name": "bar"},
                "predicate": {"not_exists": {}}
              }]
            }
          ]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter =
    OfferConstraintsFilter::create(std::move(*constraints));

  ASSERT_SOME(filter);

  EXPECT_FALSE(filter->isAgentExcluded("roleA", slaveInfoWithAttributes("")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("baz:123")));

  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:123")));

  EXPECT_FALSE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("bar:123;foo:456")));
}


// Tests offer constraints for two roles.
TEST(OfferConstraintsFilter, TwoRoles)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "foo"},
              "predicate": {"exists": {}}
            }]
          }]
        },
        "roleB": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"exists": {}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter =
    OfferConstraintsFilter::create(std::move(*constraints));

  ASSERT_SOME(filter);

  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:123")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("foo:123")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleB", slaveInfoWithAttributes("bar:123")));

  EXPECT_TRUE(
      filter->isAgentExcluded("roleB", slaveInfoWithAttributes("foo:123")));
}
