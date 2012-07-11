/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <gmock/gmock.h>

#include <map>
#include <string>

#include "common/option.hpp"

#include "flags/flags.hpp"

class TestFlags : public FlagsBase
{
public:
  TestFlags()
  {
    add(&name1,
        "name1",
        "Set name1",
        "ben folds");

    add(&name2,
        "name2",
        "Set name2",
        42);

    add(&name3,
        "name3",
        "Set name3",
        false);

    add(&name4,
        "name4",
        "Set name4");

    add(&name5,
        "name5",
        "Set name5");
  }

  std::string name1;
  int name2;
  bool name3;
  Option<bool> name4;
  Option<bool> name5;
};


TEST(FlagsTest, Load)
{
  TestFlags flags;

  std::map<std::string, Option<std::string> > values;

  values["name1"] = Option<std::string>::some("billy joel");
  values["name2"] = Option<std::string>::some("43");
  values["name3"] = Option<std::string>::some("false");
  values["no-name4"] = Option<std::string>::none();
  values["name5"] = Option<std::string>::none();

  flags.load(values);

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_EQ(false, flags.name3);
  ASSERT_TRUE(flags.name4.isSome());
  EXPECT_EQ(false, flags.name4.get());
  ASSERT_TRUE(flags.name5.isSome());
  EXPECT_EQ(true, flags.name5.get());
}


TEST(FlagsTest, Add)
{
  TestFlags flags;

  Option<std::string> name6;

  flags.add(&name6,
            "name6",
            "Also set name6");

  bool name7;

  flags.add(&name7,
            "name7",
            "Also set name7",
            true);

  Option<std::string> name8;

  flags.add(&name8,
            "name8",
            "Also set name8");

  std::map<std::string, Option<std::string> > values;

  values["name6"] = Option<std::string>::some("ben folds");
  values["no-name7"] = Option<std::string>::none();

  flags.load(values);

  ASSERT_TRUE(name6.isSome());
  EXPECT_EQ("ben folds", name6.get());

  EXPECT_EQ(false, name7);

  ASSERT_TRUE(name8.isNone());
}


TEST(FlagsTest, Flags)
{
  Flags<TestFlags> flags;

  std::map<std::string, Option<std::string> > values;

  values["name1"] = Option<std::string>::some("billy joel");
  values["name2"] = Option<std::string>::some("43");
  values["name3"] = Option<std::string>::some("false");
  values["no-name4"] = Option<std::string>::none();
  values["name5"] = Option<std::string>::none();

  flags.load(values);

  TestFlags testFlags = flags;

  EXPECT_EQ("billy joel", testFlags.name1);
  EXPECT_EQ(43, testFlags.name2);
  EXPECT_EQ(false, testFlags.name3);
  ASSERT_TRUE(testFlags.name4.isSome());
  EXPECT_EQ(false, testFlags.name4.get());
  ASSERT_TRUE(testFlags.name5.isSome());
  EXPECT_EQ(true, testFlags.name5.get());
}
