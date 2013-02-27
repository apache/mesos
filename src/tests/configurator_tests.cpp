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

#include <gtest/gtest.h>

#include <fstream>

#include <boost/lexical_cast.hpp>

#include "configurator/configurator.hpp"

#include "tests/utils.hpp"

using boost::lexical_cast;

using std::ofstream;
using std::string;

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;


class ConfiguratorTest : public TemporaryDirectoryTest
{
};


TEST_F(ConfiguratorTest, Environment)
{
  setenv("MESOS_TEST", "working", true);
  Configurator conf;
  conf.load();
  unsetenv("MESOS_TEST");

  EXPECT_EQ("working", conf.getConfiguration()["test"]);
}


TEST_F(ConfiguratorTest, DefaultOptions)
{
  const int ARGC = 5;
  char* argv[ARGC];
  argv[0] = (char*) "bin/filename";
  argv[1] = (char*) "--test1=501";
  argv[2] = (char*) "--test2";
  argv[3] = (char*) "--excp=txt";
  argv[4] = (char*) "--test8=foo";

  Configurator conf;

  EXPECT_NO_THROW(conf.addOption<int>("test1", "Testing option", 500));
  EXPECT_NO_THROW(conf.addOption<bool>("test2", "Another tester", 0));
  EXPECT_NO_THROW(conf.addOption<long>("test3", "Tests the default", 2010));
  EXPECT_NO_THROW(conf.addOption<string>("test4", "Option without default"));
  EXPECT_NO_THROW(conf.addOption<string>("test5", "Option with a default",
                                         "default"));
  EXPECT_NO_THROW(conf.addOption<bool>("test6", "Toggler...", false));
  EXPECT_NO_THROW(conf.addOption<bool>("test7", "Toggler...", true));
  EXPECT_NO_THROW(conf.addOption<string>("test8", "Overridden default",
                                         "default"));
  EXPECT_NO_THROW(conf.load(ARGC, argv));

  EXPECT_NO_THROW(conf.addOption<int>("excp", "Exception tester.", 50));
  EXPECT_THROW(conf.validate(), ConfigurationException);
  conf.getConfiguration()["excp"] = "27";
  EXPECT_NO_THROW(conf.validate());

  EXPECT_EQ("501",     conf.getConfiguration()["test1"]);
  EXPECT_EQ("1",       conf.getConfiguration()["test2"]);
  EXPECT_EQ("2010",    conf.getConfiguration()["test3"]);
  EXPECT_EQ("",        conf.getConfiguration()["test4"]);
  EXPECT_EQ("default", conf.getConfiguration()["test5"]);
  EXPECT_EQ("27",      conf.getConfiguration()["excp"]);
  EXPECT_EQ("0",       conf.getConfiguration()["test6"]);
  EXPECT_EQ("1",       conf.getConfiguration()["test7"]);
  EXPECT_EQ("foo",     conf.getConfiguration()["test8"]);
}


TEST_F(ConfiguratorTest, CommandLine)
{
  const int ARGC = 12;
  char* argv[ARGC];
  argv[0] =  (char*) "bin/filename";
  argv[1] =  (char*) "--test1=text1";
  argv[2] =  (char*) "--test2";
  argv[3] =  (char*) "text2";
  argv[4] =  (char*) "-N";
  argv[5] =  (char*) "-25";
  argv[6] =  (char*) "--cAsE=4";
  argv[7] =  (char*) "--space=Long String";
  argv[8] =  (char*) "--bool1";
  argv[9] =  (char*) "--no-bool2";
  argv[10] = (char*) "-a";
  argv[11] = (char*) "-no-b";

  Configurator conf;
  EXPECT_NO_THROW(conf.addOption<int>("negative", 'N', "some val", -30));
  EXPECT_NO_THROW(conf.addOption<string>("test1", "textual value", "text2"));
  EXPECT_NO_THROW(conf.addOption<bool>("bool1", "toggler", false));
  EXPECT_NO_THROW(conf.addOption<bool>("bool2", 'z', "toggler", true));
  EXPECT_NO_THROW(conf.addOption<bool>("bool3", 'a', "toggler", false));
  EXPECT_NO_THROW(conf.addOption<bool>("bool4", 'b', "toggler", true));

  EXPECT_NO_THROW( conf.load(ARGC, argv) );

  EXPECT_EQ("text1",       conf.getConfiguration()["test1"]);
  EXPECT_EQ("1",           conf.getConfiguration()["test2"]);
  EXPECT_EQ("-25",         conf.getConfiguration()["negative"]);
  EXPECT_EQ("4",           conf.getConfiguration()["case"]);
  EXPECT_EQ("Long String", conf.getConfiguration()["space"]);
  EXPECT_EQ("1",           conf.getConfiguration()["bool1"]);
  EXPECT_EQ("0",           conf.getConfiguration()["bool2"]);
  EXPECT_EQ("1",           conf.getConfiguration()["bool3"]);
  EXPECT_EQ("0",           conf.getConfiguration()["bool4"]);
}


// Check whether specifying just MESOS_CONF allows a config file to be loaded
TEST_F(ConfiguratorTest, ConfigFileWithConfDir)
{
  ASSERT_SOME(os::mkdir("conf2"));
  ASSERT_SOME(os::write("conf2/mesos.conf",
                        "test3=shake # sugar bomb\n"
                        "# just a comment\n"
                        "test4=milk\n"));

  setenv("MESOS_CONF", "conf2", 1);
  Configurator conf;
  EXPECT_NO_THROW( conf.load() );
  unsetenv("MESOS_CONF");

  EXPECT_EQ("shake", conf.getConfiguration()["test3"]);
  EXPECT_EQ("milk",  conf.getConfiguration()["test4"]);
}


// Check that when we specify a conf directory on the command line,
// we load values from the config file first and then the command line
TEST_F(ConfiguratorTest, CommandLineConfFlag)
{
  ASSERT_SOME(os::mkdir("bin"));
  ASSERT_SOME(os::mkdir("conf2"));
  ASSERT_SOME(os::write("conf2/mesos.conf",
                        "a=1\n"
                        "b=2\n"
                        "c=3"));

  const int ARGC = 4;
  char* argv[ARGC];
  argv[0] = (char*) "bin/filename";
  argv[1] = (char*) "--conf=conf2";
  argv[2] = (char*) "--b=overridden";
  argv[3] = (char*) "--d=fromCmdLine";

  Configurator conf;
  EXPECT_NO_THROW( conf.load(ARGC, argv) );

  EXPECT_EQ("1",           conf.getConfiguration()["a"]);
  EXPECT_EQ("overridden",  conf.getConfiguration()["b"]);
  EXPECT_EQ("3",           conf.getConfiguration()["c"]);
  EXPECT_EQ("fromCmdLine", conf.getConfiguration()["d"]);
}


// Check that variables are loaded with the correct priority when an
// environment variable, a config file element , and a config flag
// are all present. Command line flags should have the highest priority,
// second should be environment variables, and last should be the file.
TEST_F(ConfiguratorTest, LoadingPriorities)
{
  ASSERT_SOME(os::mkdir("bin"));
  ASSERT_SOME(os::mkdir("conf"));
  ASSERT_SOME(os::write("conf/mesos.conf",
                        "a=fromFile\n"
                        "b=fromFile\n"
                        "c=fromFile\n"
                        "d=fromFile\n"));

  // Set environment to contain parameters a and b
  setenv("MESOS_A", "fromEnv", 1);
  setenv("MESOS_B", "fromEnv", 1);
  setenv("MESOS_CONF", "conf", 1);

  // Pass parameters a and c from the command line
  const int ARGC = 3;
  char* argv[ARGC];
  argv[0] = (char*) "bin/filename";
  argv[1] = (char*) "--a=fromCmdLine";
  argv[2] = (char*) "--c=fromCmdLine";

  Configurator conf;
  EXPECT_NO_THROW(conf.load(ARGC, argv));

  // Clear the environment vars set above
  unsetenv("MESOS_A");
  unsetenv("MESOS_B");
  unsetenv("MESOS_CONF");

  // Check that every variable is obtained from the highest-priority location
  // (command line > env > file)
  EXPECT_EQ("fromCmdLine", conf.getConfiguration()["a"]);
  EXPECT_EQ("fromEnv",     conf.getConfiguration()["b"]);
  EXPECT_EQ("fromCmdLine", conf.getConfiguration()["c"]);
  EXPECT_EQ("fromFile",    conf.getConfiguration()["d"]);
}


// Check that spaces before and after the = signs in config files are ignored
TEST_F(ConfiguratorTest, ConfigFileSpacesIgnored)
{
  ASSERT_SOME(os::mkdir("conf"));
  ASSERT_SOME(os::write("conf/mesos.conf",
                        "test1=coffee # beans are tasty\n"
                        "# just a comment\n"
                        "  \t # comment with spaces in front\n"
                        "\n"
                        "test2 =tea\n"
                        "test3=  water\n"
                        "   test4 =  milk\n"
                        "  test5 =  hot  chocolate\t\n"
                        "\ttest6 =  juice# #\n"));

  Configurator conf;
  setenv("MESOS_CONF", "conf", 1);
  EXPECT_NO_THROW(conf.load());
  unsetenv("MESOS_CONF");

  EXPECT_EQ("coffee",         conf.getConfiguration()["test1"]);
  EXPECT_EQ("tea",            conf.getConfiguration()["test2"]);
  EXPECT_EQ("water",          conf.getConfiguration()["test3"]);
  EXPECT_EQ("milk",           conf.getConfiguration()["test4"]);
  EXPECT_EQ("hot  chocolate", conf.getConfiguration()["test5"]);
  EXPECT_EQ("juice",          conf.getConfiguration()["test6"]);
}


// Check that exceptions are thrown on invalid config file
TEST_F(ConfiguratorTest, MalformedConfigFile)
{
  ASSERT_SOME(os::mkdir("conf"));
  ASSERT_SOME(os::write("conf/mesos.conf",
                        "test1=coffee\n"
                        "JUNK\n"
                        "test2=tea\n"));

  setenv("MESOS_CONF", "conf", 1);
  Configurator conf;
  EXPECT_THROW(conf.load(), ConfigurationException);
  unsetenv("MESOS_CONF");
}
