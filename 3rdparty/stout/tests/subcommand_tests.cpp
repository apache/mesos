// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/subcommand.hpp>

using std::string;
using std::vector;


class TestSubcommand : public Subcommand
{
public:
  struct Flags : public virtual flags::FlagsBase
  {
    Flags()
    {
      add(&Flags::b, "b", "bool");
      add(&Flags::i, "i", "int");
      add(&Flags::s, "s", "string");
      add(&Flags::s2, "s2", "string with single quote");
      add(&Flags::s3, "s3", "string with double quote");
      add(&Flags::d, "d", "Duration");
      add(&Flags::y, "y", "Bytes");
      add(&Flags::j, "j", "JSON::Object");
    }

    void populate()
    {
      b = true;
      i = 42;
      s = "hello";
      s2 = "we're";
      s3 = "\"geek\"";
      d = Seconds(10);
      y = Bytes(100);

      JSON::Object object;
      object.values["strings"] = "string";
      object.values["integer1"] = 1;
      object.values["integer2"] = -1;
      object.values["double1"] = 1;
      object.values["double2"] = -1;
      object.values["double3"] = -1.42;

      JSON::Object nested;
      nested.values["string"] = "string";
      object.values["nested"] = nested;

      JSON::Array array;
      array.values.push_back(nested);
      object.values["array"] = array;

      j = object;
    }

    Option<bool> b;
    Option<int> i;
    Option<string> s;
    Option<string> s2;
    Option<string> s3;
    Option<Duration> d;
    Option<Bytes> y;
    Option<JSON::Object> j;
  };

  explicit TestSubcommand(const string& name) : Subcommand(name) {}

  Flags flags;

protected:
  int execute() override { return 0; }
  flags::FlagsBase* getFlags() override { return &flags; }
};


// Generates a vector of arguments from flags.
static vector<string> getArgv(const flags::FlagsBase& flags)
{
  vector<string> argv;
  foreachpair (const string& name, const flags::Flag& flag, flags) {
    Option<string> value = flag.stringify(flags);
    if (value.isSome()) {
      argv.push_back("--" + name + "=" + value.get());
    }
  }
  return argv;
}


TEST(SubcommandTest, Flags)
{
  TestSubcommand::Flags flags;
  flags.populate();

  // Construct the command line arguments.
  vector<string> _argv = getArgv(flags);
  int argc = static_cast<int>(_argv.size()) + 2;
  char** argv = new char*[argc];
  argv[0] = (char*) "command";
  argv[1] = (char*) "subcommand";
  for (int i = 2; i < argc; i++) {
    argv[i] = ::strdup(_argv[i - 2].c_str());
  }

  TestSubcommand subcommand("subcommand");

  ASSERT_EQ(0, Subcommand::dispatch(
      None(),
      argc,
      argv,
      &subcommand));

  EXPECT_EQ(flags.b, subcommand.flags.b);
  EXPECT_EQ(flags.i, subcommand.flags.i);
  EXPECT_EQ(flags.s, subcommand.flags.s);
  EXPECT_EQ(flags.s2, subcommand.flags.s2);
  EXPECT_EQ(flags.s3, subcommand.flags.s3);
  EXPECT_EQ(flags.d, subcommand.flags.d);
  EXPECT_EQ(flags.y, subcommand.flags.y);
  EXPECT_EQ(flags.j, subcommand.flags.j);

  for (int i = 2; i < argc; i++) {
    ::free(argv[i]);
  }
  delete[] argv;
}


TEST(SubcommandTest, Dispatch)
{
  TestSubcommand subcommand("subcommand");
  TestSubcommand subcommand2("subcommand2");

  int argc = 2;
  char* argv[] = {
    (char*) "command",
    (char*) "subcommand"
  };

  EXPECT_EQ(1, Subcommand::dispatch(
      None(),
      argc,
      argv,
      &subcommand2));

  // Duplicated subcommand names.
  EXPECT_EQ(1, Subcommand::dispatch(
      None(),
      argc,
      argv,
      &subcommand,
      &subcommand));

  EXPECT_EQ(0, Subcommand::dispatch(
      None(),
      argc,
      argv,
      &subcommand,
      &subcommand2));
}
