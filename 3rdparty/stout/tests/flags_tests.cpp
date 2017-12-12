// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <map>
#include <string>

#include <gmock/gmock.h>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/some.hpp>
#include <stout/utils.hpp>

#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

using flags::Flag;
using flags::FlagsBase;
using flags::Warnings;

using std::cout;
using std::endl;
using std::string;
using std::map;

using utils::arraySize;

// Just used to test that the default implementation
// of --help and 'usage()' works as intended.
class EmptyFlags : public virtual FlagsBase {};


class TestFlagsBase : public virtual FlagsBase
{
public:
  TestFlagsBase()
  {
    add(&TestFlagsBase::name1,
        "name1",
        "Set name1",
        "ben folds");

    add(&TestFlagsBase::name2,
        "name2",
        "Set name2",
        42);

    add(&TestFlagsBase::name3,
        "name3",
        "Set name3",
        false);

    add(&TestFlagsBase::name4,
        "name4",
        "Set name4");

    add(&TestFlagsBase::name5,
        "name5",
        "Set name5");
  }

  string name1;
  int name2;
  bool name3;
  Option<bool> name4;
  Option<bool> name5;
};


TEST(FlagsTest, Load)
{
  TestFlagsBase flags;

  const map<string, Option<string>> values = {
    {"name1", Some("billy joel")},
    {"name2", Some("43")},
    {"name3", Some("false")},
    {"no-name4", None()},
    {"name5", None()}
  };

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
  class TestFlags : public virtual FlagsBase
  {
  public:
    TestFlags()
    {
      add(&TestFlags::name1, "name1", "Also set name1");
      add(&TestFlags::name2, "name2", "Also set name2", true);
      add(&TestFlags::name3, "name3", "Also set name3");
      add(&TestFlags::name4, "name4", "Also set name4");
    }

    Option<string> name1;
    bool name2;
    Option<string> name3;
    Option<string> name4;
  };

  TestFlags flags;

  const map<string, Option<string>> values = {
    {"name1", Some("ben folds")},
    {"no-name2", None()},
    {"name4", Some("")}
  };

  flags.load(values);

  ASSERT_SOME(flags.name1);
  EXPECT_EQ("ben folds", flags.name1.get());

  EXPECT_FALSE(flags.name2);

  EXPECT_NONE(flags.name3);

  ASSERT_SOME(flags.name4);
  EXPECT_EQ("", flags.name4.get());
}


TEST(FlagsTest, Alias)
{
  class TestFlags : public virtual FlagsBase
  {
  public:
    TestFlags()
    {
      add(
        &TestFlags::name1,
        "name1",
        Some("alias1"),
        "Also set name1");

      add(&TestFlags::name2,
        "name2",
        Some("alias2"),
        "Also set name2",
        true);

      add(
        &TestFlags::name3,
        "name3",
        Some("alias3"),
        "Also set name3",
        "value8");
    }

    Option<string> name1;
    bool name2;
    string name3;
  };

  TestFlags flags;

  // Load with alias names.
  const map<string, Option<string>> values = {
     {"alias1", Some("foo")},
     {"no-alias2", None()},
     {"alias3", Some("bar")}
  };

  flags.load(values);

  ASSERT_SOME(flags.name1);
  EXPECT_EQ("foo", flags.name1.get());

  EXPECT_FALSE(flags.name2);

  EXPECT_EQ("bar", flags.name3);
}


TEST(FlagsTest, Flags)
{
  TestFlagsBase flags;

  const map<string, Option<string>> values = {
    {"name1", Some("billy joel")},
    {"name2", Some("43")},
    {"name3", Some("false")},
    {"no-name4", None()},
    {"name5", None()}
  };

  flags.load(values);

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_FALSE(flags.name3);
  ASSERT_SOME(flags.name4);
  EXPECT_FALSE(flags.name4.get());
  ASSERT_SOME(flags.name5);
  EXPECT_TRUE(flags.name5.get());
}


TEST(FlagsTest, LoadFromEnvironment)
{
  TestFlagsBase flags;

  os::setenv("FLAGSTEST_name1", "billy joel");
  os::setenv("FLAGSTEST_name2", "43");
  os::setenv("FLAGSTEST_no-name3", "");
  os::setenv("FLAGSTEST_no-name4", "");
  os::setenv("FLAGSTEST_name5", "");

  Try<Warnings> load = flags.load("FLAGSTEST_");
  EXPECT_SOME(load);
  EXPECT_TRUE(load->warnings.empty());

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_FALSE(flags.name3);
  ASSERT_SOME(flags.name4);
  EXPECT_FALSE(flags.name4.get());
  ASSERT_SOME(flags.name5);
  EXPECT_TRUE(flags.name5.get());

  os::unsetenv("FLAGSTEST_name1");
  os::unsetenv("FLAGSTEST_name2");
  os::unsetenv("FLAGSTEST_no-name3");
  os::unsetenv("FLAGSTEST_no-name4");
  os::unsetenv("FLAGSTEST_name5");
}


TEST(FlagsTest, LoadFromCommandLine)
{
  TestFlagsBase flags;

  const char* argv[] = {
    "/path/to/program",
    "--name1=billy joel",
    "--name2=43",
    "--no-name3",
    "--no-name4",
    "--name5"
  };
  const int argc = static_cast<int>(arraySize(argv));

  Try<Warnings> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_SOME(load);
  EXPECT_TRUE(load->warnings.empty());

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_FALSE(flags.name3);
  ASSERT_SOME(flags.name4);
  EXPECT_FALSE(flags.name4.get());
  ASSERT_SOME(flags.name5);
  EXPECT_TRUE(flags.name5.get());
}


TEST(FlagsTest, LoadFromCommandLineWithNonFlags)
{
  TestFlagsBase flags;

  const char* argv[] = {
    "/path/to/program",
    "more",
    "--name1=billy joel",
    "stuff",
    "at",
    "--name2=43",
    "--no-name3",
    "--no-name4",
    "--name5",
    "the",
    "end"
  };
  const int argc = static_cast<int>(arraySize(argv));

  Try<Warnings> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_SOME(load);
  EXPECT_TRUE(load->warnings.empty());

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_FALSE(flags.name3);
  ASSERT_SOME(flags.name4);
  EXPECT_FALSE(flags.name4.get());
  ASSERT_SOME(flags.name5);
  EXPECT_TRUE(flags.name5.get());
}


TEST(FlagsTest, LoadFromCommandLineWithDashDash)
{
  TestFlagsBase flags;

  const char* argv[] = {
    "/path/to/program",
    "more",
    "--name1=billy joel",
    "stuff",
    "at",
    "--name2=43",
    "--no-name3",
    "--",
    "--no-name4",
    "--name5",
    "the"
  };
  const int argc = static_cast<int>(arraySize(argv));

  Try<Warnings> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_SOME(load);
  EXPECT_TRUE(load->warnings.empty());

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_FALSE(flags.name3);
  ASSERT_NONE(flags.name4);
  ASSERT_NONE(flags.name5);
}


TEST(FlagsTest, LoadFromCommandLineAndUpdateArgcArgv)
{
  TestFlagsBase flags;

  char* argv[] = {
    (char*)"/path/to/program",
    (char*)"more",
    (char*)"--name1=billy joel",
    (char*)"stuff",
    (char*)"at",
    (char*)"--name2=43",
    (char*)"--no-name3",
    (char*)"--",
    (char*)"--no-name4",
    (char*)"--name5",
    (char*)"the"
  };
  int argc = static_cast<int>(arraySize(argv));

  // Need a temporary since some compilers want to treat the type of
  // 'argv' as 'char *(*)[argc]' since the size of the array is known.
  char** _argv = argv;

  Try<Warnings> load = flags.load("FLAGSTEST_", &argc, &_argv);
  EXPECT_SOME(load);
  EXPECT_TRUE(load->warnings.empty());

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_FALSE(flags.name3);
  ASSERT_NONE(flags.name4);
  ASSERT_NONE(flags.name5);

  EXPECT_EQ(7, argc);
  EXPECT_STREQ("/path/to/program", argv[0]);
  EXPECT_STREQ("more", argv[1]);
  EXPECT_STREQ("stuff", argv[2]);
  EXPECT_STREQ("at", argv[3]);
  EXPECT_STREQ("--no-name4", argv[4]);
  EXPECT_STREQ("--name5", argv[5]);
  EXPECT_STREQ("the", argv[6]);
  EXPECT_EQ(nullptr, argv[7]);
}


TEST(FlagsTest, Stringification)
{
  class TestFlags : public virtual TestFlagsBase
  {
  public:
    TestFlags()
    {
      add(&TestFlags::name6, "name6", "Also set name6", Milliseconds(42));
      add(&TestFlags::name7, "name7", "Optional name7");
      add(&TestFlags::name8, "name8", "Optional name8");
    }

    Duration name6;
    Option<bool> name7;
    Option<bool> name8;
  };

  TestFlags flags;

  const map<string, Option<string>> values = {
    {"name2", Some("43")},
    {"no-name4", None()},
    {"name5", None()}
  };

  flags.load(values);

  foreachpair (const string& name, const Flag& flag, flags) {
    Option<string> value = flag.stringify(flags);
    if (name == "name1") {
      ASSERT_SOME(value);
      EXPECT_EQ("ben folds", value.get());
    } else if (name == "name2") {
      ASSERT_SOME(value);
      EXPECT_EQ("43", value.get());
    } else if (name == "name3") {
      ASSERT_SOME(value);
      EXPECT_EQ("false", value.get());
    } else if (name == "name4") {
      ASSERT_SOME(value);
      EXPECT_EQ("false", value.get());
    } else if (name == "name5") {
      ASSERT_SOME(value);
      EXPECT_EQ("true", value.get());
    } else if (name == "name6") {
      ASSERT_SOME(value);
      EXPECT_EQ("42ms", value.get());
    } else if (name == "name7") {
      ASSERT_NONE(value);
    } else if (name == "name8") {
      ASSERT_NONE(value);
    }
  }
}


TEST(FlagsTest, EffectiveName)
{
  class TestFlags : public virtual FlagsBase
  {
  public:
    TestFlags()
    {
       add(
         &TestFlags::name1,
         "name1",
         Some("alias1"),
         "Also set name1");

       add(
         &TestFlags::name2,
         "name2",
         Some("alias2"),
         "Also set name2",
         "value7");
    }

    Option<string> name1;
    string name2;
  };

  TestFlags flags;

  // Only load "name1" flag explicitly.
  const map<string, Option<string>> values = {
    {"alias1", Some("value6")}
  };

  flags.load(values);

  foreachvalue (const Flag& flag, flags) {
    if (flag.name == "name1") {
      EXPECT_EQ("alias1", flag.effective_name().value);
    } else if (flag.name == "name2") {
      EXPECT_EQ("name2", flag.effective_name().value);
    }
  }
}


TEST(FlagsTest, DeprecationWarning)
{
  class TestFlags : public virtual FlagsBase
  {
  public:
    TestFlags()
    {
      add(
        &TestFlags::name,
        "name",
        flags::DeprecatedName("alias"),
        "Also set name");
    }

    Option<string> name;
  };

  TestFlags flags;

  const map<string, Option<string>> values = {
    {"alias", Some("value6")}
  };

  Try<Warnings> load = flags.load(values);
  ASSERT_SOME(load);

  EXPECT_EQ(1u, load->warnings.size());
  EXPECT_EQ("Loaded deprecated flag 'alias'", load->warnings[0].message);
}


TEST(FlagsTest, DuplicatesFromEnvironment)
{
  TestFlagsBase flags;

  os::setenv("FLAGSTEST_name1", "ben folds");
  os::setenv("FLAGSTEST_name2", "50");

  const char* argv[] = {
    "/path/to/program",
    "--name1=billy joel"
  };
  const int argc = static_cast<int>(arraySize(argv));

  // `load(prefix, argc, argv)`.
  Try<Warnings> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_SOME(load);
  EXPECT_TRUE(load->warnings.empty());

  // The environment variables are overwritten by command line flags.
  EXPECT_EQ(flags.name1, "billy joel");
  EXPECT_EQ(flags.name2, 50);

  {
    flags = TestFlagsBase();
    std::map<std::string, std::string> values;
    values["name1"] = "billy joel";

    // `load(map<string, string>, unknowns, prefix)`.
    load = flags.load(values, false, "FLAGSTEST_");
    EXPECT_SOME(load);
    EXPECT_TRUE(load->warnings.empty());

    EXPECT_EQ(flags.name1, "billy joel");
    EXPECT_EQ(flags.name2, 50);
  }

  {
    flags = TestFlagsBase();
    std::map<std::string, Option<std::string>> values;
    values["name1"] = "billy joel";
    values["name2"] = "51";

    // `load(map<string, Option<string>, unknowns, prefix)`.
    load = flags.load(values, false, "FLAGSTEST_");

    EXPECT_SOME(load);
    EXPECT_TRUE(load->warnings.empty());

    EXPECT_EQ(flags.name1, "billy joel");
    EXPECT_EQ(flags.name2, 51);
  }

  os::unsetenv("FLAGSTEST_name1");
  os::unsetenv("FLAGSTEST_name2");
}


TEST(FlagsTest, DuplicatesFromCommandLine)
{
  TestFlagsBase flags;

  const char* argv[] = {
    "/path/to/program",
    "--name1=billy joel",
    "--name1=ben folds"
  };
  const int argc = static_cast<int>(arraySize(argv));

  // TODO(klaus1982): Simply checking for the error. Once typed errors are
  // introduced, capture it within the type system.
  Try<Warnings> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);
}


TEST(FlagsTest, AliasDuplicateFromCommandLine)
{
  class TestFlags : public virtual FlagsBase
  {
  public:
    TestFlags()
    {
      add(&TestFlags::name, "name", Some("alias"), "Also set name");
    }

    Option<string> name;
  };

  TestFlags flags;

  const char* argv[] = {
    "/path/to/program",
    "--name=billy joel",
    "--alias=ben folds"
  };
  const int argc = static_cast<int>(arraySize(argv));

  // Loading the same flag with the name and alias should be an error.
  Try<Warnings> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);
}


TEST(FlagsTest, Errors)
{
  class TestFlags : public virtual TestFlagsBase
  {
  public:
    TestFlags() { add(&TestFlags::name6, "name6", "Also set name6"); }

    Option<int> name6;
  };

  TestFlags flags;

  const int argc = 2;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";

  // Test an unknown flag.
  argv[1] = (char*) "--foo";

  Try<Warnings> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Failed to load unknown flag 'foo'", load.error());

  // Now try an unknown flag with a value.
  argv[1] = (char*) "--foo=value";

  load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Failed to load unknown flag 'foo'", load.error());

  // Now try an unknown flag with a 'no-' prefix.
  argv[1] = (char*) "--no-foo";

  load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Failed to load unknown flag 'foo' via 'no-foo'", load.error());

  // Now test a boolean flag using the 'no-' prefix _and_ a value.
  argv[1] = (char*) "--no-name3=value";

  load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Failed to load boolean flag 'name3' via "
            "'no-name3' with value 'value'", load.error());

  // Now test a boolean flag that couldn't be parsed.
  argv[1] = (char*) "--name3=value";

  load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Failed to load flag 'name3': Failed to load value 'value': "
            "Expecting a boolean (e.g., true or false)", load.error());

  // Now test a non-boolean flag without a value.
  argv[1] = (char*) "--name1";

  load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Failed to load non-boolean flag 'name1': "
            "Missing value", load.error());

  // Now test a non-boolean flag using the 'no-' prefix.
  argv[1] = (char*) "--no-name2";

  load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Failed to load non-boolean flag 'name2' "
            "via 'no-name2'", load.error());

  // Now test a non-boolean flag using empty string value.
  argv[1] = (char*) "--name6=";

  load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Failed to load flag 'name6': Failed to load value '': "
            "Failed to convert into required type", load.error());
}


// This test confirms that loading flags when a required flag is missing will
// result in an error.
TEST(FlagsTest, MissingRequiredFlag)
{
  class TestFlags : public virtual TestFlagsBase
  {
  public:
    TestFlags()
    {
      add(
        &TestFlags::requiredFlag,
        "required_flag",
        "This flag is required and has no default value.");
    }

    // A required flag which must be set and has no default value.
    string requiredFlag;
  };

  TestFlags flags;

  const int argc = 2;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";
  argv[1] = (char*) "--name1=name";

  Try<Warnings> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ(
      "Flag 'required_flag' is required, but it was not provided",
      load.error());
}


TEST(FlagsTest, Validate)
{
  // To provide validation functions.
  class ValidatingTestFlags : public virtual FlagsBase
  {
  public:
    ValidatingTestFlags()
    {
      add(&ValidatingTestFlags::duration,
          "duration",
          "Duration to test validation",
          Seconds(10),
          [](const Duration& value) -> Option<Error> {
            if (value > Hours(1)) {
              return Error("Expected --duration to be less than 1 hour");
            }
            return None();
          });
    }

    Duration duration;
  };

  ValidatingTestFlags flags;

  const char* argv[] = {
    "/path/to/program",
    "--duration=2hrs"
  };
  const int argc = static_cast<int>(arraySize(argv));

  Try<Warnings> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Expected --duration to be less than 1 hour", load.error());
}


TEST(FlagsTest, Usage)
{
  class TestFlags : public virtual TestFlagsBase
  {
  public:
    TestFlags()
    {
      add(&TestFlags::name6, "z6", Some("a6"), "Also set name6");
      add(&TestFlags::name7, "z7", Some("a7"), "Also set name7", true);
      add(&TestFlags::name8, "z8", Some("a8"), "Also set name8", "value8");
    }

    Option<string> name6;
    bool name7;
    string name8;
  };

  TestFlags flags;

  EXPECT_EQ(
      "Usage:  [options]\n"
      "\n"
      "  --[no-]help                Prints this help message (default: false)\n"
      "  --name1=VALUE              Set name1 (default: ben folds)\n"
      "  --name2=VALUE              Set name2 (default: 42)\n"
      "  --[no-]name3               Set name3 (default: false)\n"
      "  --[no-]name4               Set name4\n"
      "  --[no-]name5               Set name5\n"
      "  --z6=VALUE, --a6=VALUE     Also set name6\n"
      "  --[no-]z7, --[no-]a7       Also set name7 (default: true)\n"
      "  --z8=VALUE, --a8=VALUE     Also set name8 (default: value8)\n",
      flags.usage());
}


TEST(FlagsTest, UsageMessage)
{
  TestFlagsBase flags;
  flags.setUsageMessage("This is a test");

  EXPECT_EQ(
      "This is a test\n"
      "\n"
      "  --[no-]help       Prints this help message (default: false)\n"
      "  --name1=VALUE     Set name1 (default: ben folds)\n"
      "  --name2=VALUE     Set name2 (default: 42)\n"
      "  --[no-]name3      Set name3 (default: false)\n"
      "  --[no-]name4      Set name4\n"
      "  --[no-]name5      Set name5\n",
      flags.usage());
}


TEST(FlagsTest, EmptyUsage)
{
  EmptyFlags flags;

  EXPECT_EQ(
      "Usage:  [options]\n"
      "\n"
      "  --[no-]help     Prints this help message (default: false)\n",
      flags.usage());
}


TEST(FlagsTest, ProgramName)
{
  // To test with a custom program name.
  class MyTestFlags : public virtual FlagsBase
  {
  public:
    MyTestFlags() { programName_ = "TestProgram"; }
  };

  MyTestFlags flags;

  EXPECT_EQ(
      "Usage: TestProgram [options]\n"
      "\n"
      "  --[no-]help     Prints this help message (default: false)\n",
      flags.usage());
}


TEST(FlagsTest, OptionalMessage)
{
  TestFlagsBase flags;

  EXPECT_EQ(
      "Good news: this test passed!\n"
      "\n"
      "Usage:  [options]\n"
      "\n"
      "  --[no-]help       Prints this help message (default: false)\n"
      "  --name1=VALUE     Set name1 (default: ben folds)\n"
      "  --name2=VALUE     Set name2 (default: 42)\n"
      "  --[no-]name3      Set name3 (default: false)\n"
      "  --[no-]name4      Set name4\n"
      "  --[no-]name5      Set name5\n",
      flags.usage("Good news: this test passed!"));
}


TEST(FlagsTest, BuildEnvironment)
{
  TestFlagsBase flags;
  flags.name4 = true;

  map<string, string> environment = flags.buildEnvironment("PREFIX_");

  ASSERT_EQ(1u, environment.count("PREFIX_NAME1"));
  EXPECT_EQ("ben folds", environment["PREFIX_NAME1"]);

  ASSERT_EQ(1u, environment.count("PREFIX_NAME2"));
  EXPECT_EQ("42", environment["PREFIX_NAME2"]);

  ASSERT_EQ(1u, environment.count("PREFIX_NAME3"));
  EXPECT_EQ("false", environment["PREFIX_NAME3"]);

  ASSERT_EQ(1u, environment.count("PREFIX_NAME4"));
  EXPECT_EQ("true", environment["PREFIX_NAME4"]);

  EXPECT_EQ(0u, environment.count("PREFIX_NAME5"));
}


TEST(FlagsTest, Duration)
{
  class TestFlags : public virtual FlagsBase
  {
  public:
    TestFlags()
    {
      add(&TestFlags::name1, "name1", "Amount of time", Milliseconds(100));
      add(&TestFlags::name2, "name2", "Also some amount of time");
    }

    Duration name1;
    Option<Duration> name2;
  };

  TestFlags flags;

  const map<string, Option<string>> values = {
    {"name1", Some("2mins")},
    {"name2", Some("3hrs")}
  };

  ASSERT_SOME(flags.load(values));

  EXPECT_EQ(Minutes(2), flags.name1);

  EXPECT_SOME_EQ(Hours(3), flags.name2);
}


TEST(FlagsTest, JSON)
{
  class TestFlags : public virtual FlagsBase
  {
  public:
    TestFlags() { add(&TestFlags::json, "json", "JSON string"); }

    Option<JSON::Object> json;
  };

  TestFlags flags;

  JSON::Object object;

  object.values["strings"] = "string";
  object.values["integer"] = 1;
  object.values["double"] = -1.42;

  JSON::Object nested;
  nested.values["string"] = "string";

  object.values["nested"] = nested;

  map<string, Option<string>> values;
  values["json"] = Some(stringify(object));

  ASSERT_SOME(flags.load(values));

  ASSERT_SOME_EQ(object, flags.json);
}


class FlagsFileTest : public TemporaryDirectoryTest {};


#ifndef __WINDOWS__
// This tests deprecated code that pre-dates Windows support for Mesos.
// Hence, we should not build or enable this test on Windows.
TEST_F(FlagsFileTest, JSONFile)
{
  class TestFlags : public virtual FlagsBase
  {
  public:
    TestFlags() { add(&TestFlags::json, "json", "JSON string"); }

    Option<JSON::Object> json;
  };

  TestFlags flags;

  JSON::Object object;

  object.values["strings"] = "string";
  object.values["integer"] = 1;
  object.values["double"] = -1.42;

  JSON::Object nested;
  nested.values["string"] = "string";

  object.values["nested"] = nested;

  // Write the JSON to a file.
  const string file = path::join(os::getcwd(), "file.json");
  ASSERT_SOME(os::write(file, stringify(object)));

  // Read the JSON from the file.
  map<string, Option<string>> values;
  values["json"] = Some(file);

  ASSERT_SOME(flags.load(values));

  ASSERT_SOME_EQ(object, flags.json);
}
#endif // __WINDOWS__


TEST_F(FlagsFileTest, FilePrefix)
{
  class TestFlags : public virtual FlagsBase
  {
  public:
    TestFlags()
    {
      add(&TestFlags::something, "something", "arg to be loaded from file");
    }

    Option<string> something;
  };

  TestFlags flags;

  // Write the JSON to a file.
  const string file = path::join(os::getcwd(), "file");
  ASSERT_SOME(os::write(file, "testing"));

  // Read the JSON from the file.
  map<string, Option<string>> values;
  values["something"] = Some("file://" + file);

  ASSERT_SOME(flags.load(values));

  ASSERT_SOME_EQ("testing", flags.something);
}
