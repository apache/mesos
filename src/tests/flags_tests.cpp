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

#include <stout/duration.hpp>
#include <stout/option.hpp>

#include "configurator/configuration.hpp"
#include "configurator/configurator.hpp"

#include "flags/flags.hpp"

#include "tests/utils.hpp"

using namespace flags;

class TestFlags : public virtual FlagsBase
{
public:
  TestFlags()
  {
    add(&TestFlags::name1,
        "name1",
        "Set name1",
        "ben folds");

    add(&TestFlags::name2,
        "name2",
        "Set name2",
        42);

    add(&TestFlags::name3,
        "name3",
        "Set name3",
        false);

    add(&TestFlags::name4,
        "name4",
        "Set name4");

    add(&TestFlags::name5,
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
  EXPECT_FALSE(flags.name3);
  ASSERT_SOME(flags.name4);
  EXPECT_FALSE(flags.name4.get());
  ASSERT_SOME(flags.name5);
  EXPECT_TRUE(flags.name5.get());
}


TEST(FlagsTest, Add)
{
  Flags<TestFlags> flags;

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

  ASSERT_SOME(name6);
  EXPECT_EQ("ben folds", name6.get());

  EXPECT_FALSE(name7);

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

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_FALSE(flags.name3);
  ASSERT_SOME(flags.name4);
  EXPECT_FALSE(flags.name4.get());
  ASSERT_SOME(flags.name5);
  EXPECT_TRUE(flags.name5.get());
}


TEST(FlagsTest, Configurator)
{
  Flags<TestFlags> flags;

  int argc = 6;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";
  argv[1] = (char*) "--name1=billy joel";
  argv[2] = (char*) "--name2=43";
  argv[3] = (char*) "--no-name3";
  argv[4] = (char*) "--no-name4";
  argv[5] = (char*) "--name5";

  mesos::internal::Configurator configurator(flags);
  mesos::internal::Configuration configuration;
  try {
    configuration = configurator.load(argc, argv);
  } catch (mesos::internal::ConfigurationException& e) {
    FAIL() << "Configuration error: " << e.what();
  }

  flags.load(configuration.getMap());

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_FALSE(flags.name3);
  ASSERT_SOME(flags.name4);
  EXPECT_FALSE(flags.name4.get());
  ASSERT_SOME(flags.name5);
  EXPECT_TRUE(flags.name5.get());
}


TEST(FlagsTest, Duration)
{
  Flags<TestFlags> flags;

  Duration name6;

  flags.add(&name6,
            "name6",
            "Amount of time",
            Milliseconds(100));

  Option<Duration> name7;

  flags.add(&name7,
            "name7",
            "Also some amount of time");

  std::map<std::string, Option<std::string> > values;

  values["name6"] = Option<std::string>::some("2mins");
  values["name7"] = Option<std::string>::some("3hrs");

  flags.load(values);

  EXPECT_EQ(Minutes(2), name6);

  ASSERT_SOME(name7);
  EXPECT_EQ(Hours(3), name7.get());
}
