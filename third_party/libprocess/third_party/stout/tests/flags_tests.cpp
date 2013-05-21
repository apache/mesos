#include <gmock/gmock.h>

#include <map>
#include <string>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>


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

  std::map<std::string, Option<std::string> > values;

  values["name1"] = Option<std::string>::some("billy joel");
  values["name2"] = Option<std::string>::some("43");
  values["name3"] = Option<std::string>::some("false");
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

  std::map<std::string, Option<std::string> > values;

  values["name6"] = Option<std::string>::some("2mins");
  values["name7"] = Option<std::string>::some("3hrs");

  flags.load(values);

  EXPECT_EQ(Minutes(2), name6);

  ASSERT_SOME(name7);
  EXPECT_EQ(Hours(3), name7.get());
}
