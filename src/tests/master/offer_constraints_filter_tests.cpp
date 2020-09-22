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


static Try<OfferConstraintsFilter> createFilter(OfferConstraints constraints)
{
  OfferConstraintsFilter::Options options;
  options.re2Limits.maxMem = Kilobytes(4);
  options.re2Limits.maxProgramSize = 100;
  return OfferConstraintsFilter::create(options, std::move(constraints));
}


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

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

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

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

  ASSERT_SOME(filter);

  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:123")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("foo:123")));

  EXPECT_TRUE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("foo:123;bar:456")));
}


// Tests a single TextEquals constraint on a named attribute.
TEST(OfferConstraintsFilter, NamedAttributeTextEquals)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"text_equals": {"value": "baz"}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

  ASSERT_SOME(filter);

  // Attribute exists, is a string and equals to the value in the constraint.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:baz")));

  // Attribute is not a string.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:123")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:[1-17]")));

  // Attribute has a wrong string value.
  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:zab")));

  // Attribute does not exist.
  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("foo:baz")));
}


// Tests a single TextNotEquals constraint on a named attribute.
TEST(OfferConstraintsFilter, NamedAttributeTextNotEquals)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"text_not_equals": {"value": "baz"}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

  ASSERT_SOME(filter);

  // Attribute exists, is a string and equals to the value in the constraint.
  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:baz")));

  // Attribute is not a string.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:123")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:[1-17]")));

  // Attribute has a different string value.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:zab")));

  // Attribute does not exist.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("foo:baz")));
}


// Tests that when an attribute has several values, the constraint's predicate
// is evaluated against the first one.
TEST(OfferConstraintsFilter, TwoAttributesWithTheSameName)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"text_equals": {"value": "baz"}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

  ASSERT_SOME(filter);

  // The selected attribute has a suitable value.
  EXPECT_FALSE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("bar:baz;bar:foo")));

  EXPECT_FALSE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("bar:123;bar:foo")));

  // The selected attribute has an unsuitable value.
  EXPECT_TRUE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("bar:foo;bar:baz")));

  EXPECT_TRUE(filter->isAgentExcluded(
      "roleA", slaveInfoWithAttributes("bar:foo;bar:123")));
}


// Tests a single TextMatches constraint on a named attribute.
TEST(OfferConstraintsFilter, NamedAttributeTextMatches)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"text_matches": {"regex": "[a-d]+"}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

  ASSERT_SOME(filter);

  // Attribute exists, is a text and matches the regex.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:abcd")));

  // If an attribute is not a text, the constraint is a pass-through.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:123")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:[1-17]")));

  // Attribute is a text which does not match.
  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:bcde")));

  // Attribute does not exist.
  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("foo:abcd")));
}


// Tests a single TextNotMatches constraint on a named attribute.
TEST(OfferConstraintsFilter, NamedAttributeTextNotMatches)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"text_not_matches": {"regex": "[a-d]+"}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

  ASSERT_SOME(filter);

  // Attribute exists, is a text and matches the regex.
  EXPECT_TRUE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:abcd")));

  // If an attribute is not a text, the constraint is a pass-through.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:123")));

  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:[1-17]")));

  // Attribute is a text which does not match.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("bar:bcde")));

  // Attribute does not exist.
  EXPECT_FALSE(
      filter->isAgentExcluded("roleA", slaveInfoWithAttributes("foo:abcd")));
}


// Tests an invalid TextMatches constraint.
TEST(OfferConstraintsFilter, InvalidTextMatches)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"text_matches": {"regex": "[a-d"}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  ASSERT_ERROR(createFilter(*constraints));
}


// Tests an invalid TextNotMatches constraint.
TEST(OfferConstraintsFilter, InvalidTextNotMatches)
{
  Try<OfferConstraints> constraints = OfferConstraintsFromJSON(R"~(
    {
      "role_constraints": {
        "roleA": {
          "groups": [{
            "attribute_constraints": [{
              "selector": {"attribute_name": "bar"},
              "predicate": {"text_not_matches": {"regex": "[a-d"}}
            }]
          }]
        }
      }
    })~");

  ASSERT_SOME(constraints);

  ASSERT_ERROR(createFilter(*constraints));
}


// Tests that the constraints cannot specify a regex which will result in a too
// complex RE2 regex program.
TEST(OfferConstraintsFilter, RegexTooComplex)
{
  auto regexConstraints = [](const string& regex) {
    return OfferConstraintsFromJSON(
        R"~(
      {
        "role_constraints": {
          "roleA": {
            "groups": [{
              "attribute_constraints": [{
                "selector": {"attribute_name": "bar"},
                "predicate": {"text_not_matches": {"regex": ")~" +
        regex + R"~("}}
              }]
            }]
          }
        }
      })~");
  };

  {
    Try<OfferConstraints> good = regexConstraints("(a+){10}");
    ASSERT_SOME(good);
    ASSERT_SOME(createFilter(*good));
  }

  {
    // This regexp can be compiled (fits into a memory limit) but results in
    // a too large program.
    Try<OfferConstraints> tooComplex = regexConstraints("(a+){50}");
    ASSERT_SOME(tooComplex);

    Try<OfferConstraintsFilter> filter = createFilter(*tooComplex);
    ASSERT_ERROR(filter);
    ASSERT_TRUE(strings::contains(filter.error(), "too complex"));
  }

  {
    // This regexp does not even fit into a memory limit.
    Try<OfferConstraints> tooLarge = regexConstraints("(a+){500}");
    ASSERT_SOME(tooLarge);

    Try<OfferConstraintsFilter> filter = createFilter(*tooLarge);
    ASSERT_ERROR(filter);
    ASSERT_TRUE(strings::contains(
        filter.error(), "pattern too large - compile failed"));
  }
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

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

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

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

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

  const Try<OfferConstraintsFilter> filter = createFilter(*constraints);

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


// Tests that using default-constructed `OfferConstraints` to construct
// a filter results in a no-op filter that does not exclude any agents
// (and not, for example, in an error).
TEST(OfferConstraintsFilter, DefaultOfferConstraints)
{
  const Try<OfferConstraintsFilter> filter = createFilter(OfferConstraints{});

  ASSERT_SOME(filter);

  EXPECT_FALSE(filter->isAgentExcluded("role", SlaveInfo{}));
}
