#include <gmock/gmock.h>

#include <map>
#include <string>

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

#include <stout/tests/utils.hpp>

using namespace flags;

using std::string;
using std::map;

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

  string name1;
  int name2;
  bool name3;
  Option<bool> name4;
  Option<bool> name5;
};


TEST(FlagsTest, Load)
{
  TestFlags flags;

  map<string, Option<string> > values;

  values["name1"] = Some("billy joel");
  values["name2"] = Some("43");
  values["name3"] = Some("false");
  values["no-name4"] = None();
  values["name5"] = None();

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

  Option<string> name6;

  flags.add(&name6,
            "name6",
            "Also set name6");

  bool name7;

  flags.add(&name7,
            "name7",
            "Also set name7",
            true);

  Option<string> name8;

  flags.add(&name8,
            "name8",
            "Also set name8");

  map<string, Option<string> > values;

  values["name6"] = Some("ben folds");
  values["no-name7"] = None();

  flags.load(values);

  ASSERT_SOME(name6);
  EXPECT_EQ("ben folds", name6.get());

  EXPECT_FALSE(name7);

  ASSERT_TRUE(name8.isNone());
}


TEST(FlagsTest, Flags)
{
  TestFlags flags;

  map<string, Option<string> > values;

  values["name1"] = Some("billy joel");
  values["name2"] = Some("43");
  values["name3"] = Some("false");
  values["no-name4"] = None();
  values["name5"] = None();

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
  TestFlags flags;

  os::setenv("FLAGSTEST_name1", "billy joel");
  os::setenv("FLAGSTEST_name2", "43");
  os::setenv("FLAGSTEST_no-name3", "");
  os::setenv("FLAGSTEST_no-name4", "");
  os::setenv("FLAGSTEST_name5", "");

  Try<Nothing> load = flags.load("FLAGSTEST_");
  EXPECT_SOME(load);

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
  TestFlags flags;

  int argc = 6;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";
  argv[1] = (char*) "--name1=billy joel";
  argv[2] = (char*) "--name2=43";
  argv[3] = (char*) "--no-name3";
  argv[4] = (char*) "--no-name4";
  argv[5] = (char*) "--name5";

  Try<Nothing> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_SOME(load);

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
  TestFlags flags;

  int argc = 11;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";
  argv[1] = (char*) "more";
  argv[2] = (char*) "--name1=billy joel";
  argv[3] = (char*) "stuff";
  argv[4] = (char*) "at";
  argv[5] = (char*) "--name2=43";
  argv[6] = (char*) "--no-name3";
  argv[7] = (char*) "--no-name4";
  argv[8] = (char*) "--name5";
  argv[9] = (char*) "the";
  argv[10] = (char*) "end";

  Try<Nothing> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_SOME(load);

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
  TestFlags flags;

  int argc = 11;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";
  argv[1] = (char*) "more";
  argv[2] = (char*) "--name1=billy joel";
  argv[3] = (char*) "stuff";
  argv[4] = (char*) "at";
  argv[5] = (char*) "--name2=43";
  argv[6] = (char*) "--no-name3";
  argv[7] = (char*) "--";
  argv[8] = (char*) "--no-name4";
  argv[9] = (char*) "--name5";
  argv[10] = (char*) "the";

  Try<Nothing> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_SOME(load);

  EXPECT_EQ("billy joel", flags.name1);
  EXPECT_EQ(43, flags.name2);
  EXPECT_FALSE(flags.name3);
  ASSERT_NONE(flags.name4);
  ASSERT_NONE(flags.name5);
}


TEST(FlagsTest, LoadFromCommandLineAndUpdateArgcArgv)
{
  TestFlags flags;

  int argc = 11;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";
  argv[1] = (char*) "more";
  argv[2] = (char*) "--name1=billy joel";
  argv[3] = (char*) "stuff";
  argv[4] = (char*) "at";
  argv[5] = (char*) "--name2=43";
  argv[6] = (char*) "--no-name3";
  argv[7] = (char*) "--";
  argv[8] = (char*) "--no-name4";
  argv[9] = (char*) "--name5";
  argv[10] = (char*) "the";

  // Need a temporary since some compilers want to treat the type of
  // 'argv' as 'char *(*)[argc]' since the size of the array is known.
  char** _argv = argv;

  Try<Nothing> load = flags.load("FLAGSTEST_", &argc, &_argv);
  EXPECT_SOME(load);

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
  EXPECT_EQ(NULL, argv[7]);
}


TEST(FlagsTest, Stringification)
{
  TestFlags flags;

  Duration name6;

  flags.add(&name6,
            "name6",
            "Also set name6",
            Milliseconds(42));

  Option<bool> name7;

  flags.add(&name7,
            "name7",
            "Optional name7");

  Option<bool> name8;

  flags.add(&name8,
            "name8",
            "Optional name8");

  map<string, Option<string> > values;

  values["name2"] = Some("43");
  values["no-name4"] = None();
  values["name5"] = None();

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


TEST(FlagsTest, DuplicatesFromEnvironment)
{
  TestFlags flags;

  os::setenv("FLAGSTEST_name1", "ben folds");

  int argc = 2;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";
  argv[1] = (char*) "--name1=billy joel";

  Try<Nothing> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Duplicate flag 'name1' on command line", load.error());

  os::unsetenv("FLAGSTEST_name1");
}


TEST(FlagsTest, DuplicatesFromCommandLine)
{
  TestFlags flags;

  int argc = 3;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";
  argv[1] = (char*) "--name1=billy joel";
  argv[2] = (char*) "--name1=ben folds";

  Try<Nothing> load = flags.load("FLAGSTEST_", argc, argv);
  EXPECT_ERROR(load);

  EXPECT_EQ("Duplicate flag 'name1' on command line", load.error());
}


TEST(FlagsTest, Errors)
{
  TestFlags flags;

  int argc = 2;
  char* argv[argc];

  argv[0] = (char*) "/path/to/program";

  // Test an unknown flag.
  argv[1] = (char*) "--foo";

  Try<Nothing> load = flags.load("FLAGSTEST_", argc, argv);
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
}


TEST(FlagsTest, Usage)
{
  TestFlags flags;

  EXPECT_EQ(
      "  --name1=VALUE     Set name1 (default: ben folds)\n"
      "  --name2=VALUE     Set name2 (default: 42)\n"
      "  --[no-]name3      Set name3 (default: false)\n"
      "  --[no-]name4      Set name4\n"
      "  --[no-]name5      Set name5\n",
      flags.usage());
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

  map<string, Option<string> > values;

  values["name6"] = Some("2mins");
  values["name7"] = Some("3hrs");

  ASSERT_SOME(flags.load(values));

  EXPECT_EQ(Minutes(2), name6);

  EXPECT_SOME_EQ(Hours(3), name7);
}


TEST(FlagsTest, JSON)
{
  Flags<TestFlags> flags;

  Option<JSON::Object> json;

  flags.add(&json,
            "json",
            "JSON string");

  JSON::Object object;

  object.values["strings"] = "string";
  object.values["integer"] = 1;
  object.values["double"] = -1.42;

  JSON::Object nested;
  nested.values["string"] = "string";

  object.values["nested"] = nested;

  map<string, Option<string> > values;
  values["json"] = Some(stringify(object));

  ASSERT_SOME(flags.load(values));

  ASSERT_SOME_EQ(object, json);
}


class FlagsFileTest : public TemporaryDirectoryTest {};


TEST_F(FlagsFileTest, JSONFile)
{
  Flags<TestFlags> flags;

  Option<JSON::Object> json;

  flags.add(&json,
            "json",
            "JSON string");

  JSON::Object object;

  object.values["strings"] = "string";
  object.values["integer"] = 1;
  object.values["double"] = -1.42;

  JSON::Object nested;
  nested.values["string"] = "string";

  object.values["nested"] = nested;

  // Write the JSON to a file.
  const string& file = path::join(os::getcwd(), "file.json");
  ASSERT_SOME(os::write(file, stringify(object)));

  // Read the JSON from the file.
  map<string, Option<string> > values;
  values["json"] = Some(file);

  ASSERT_SOME(flags.load(values));

  ASSERT_SOME_EQ(object, json);
}
