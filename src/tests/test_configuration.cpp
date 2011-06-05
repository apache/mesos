#include <gtest/gtest.h>
#include <iostream>
#include <fstream>

#include <boost/lexical_cast.hpp>

#include "configuration.hpp"
#include "testing_utils.hpp"

using std::ofstream;
using std::string;
using std::vector;
using std::cout;

using boost::lexical_cast;

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::test;


TEST(ConfigurationTest, Environment)
{
  setenv("MESOS_TEST", "working", true);
  Configuration conf;
  conf.loadEnv();
  conf.loadConfigFileIfGiven();
  unsetenv("MESOS_TEST");

  EXPECT_EQ("working", conf.getParams()["test"]);
}


TEST(ConfigurationTest, DefaultOptions)
{
  const int ARGC = 3;
  char* argv[ARGC];
  argv[0] = (char*) "./filename";
  argv[1] = (char*) "--test1=text1";
  argv[2] = (char*) "--test2";

  Configuration conf;

  conf.addOption("test1", "Testing option", 500);
  conf.addOption("test2", "Another tester", 0);
  conf.addOption("test3", "Tests the default\noption.", 2010);
  conf.addOption("test4", "Option without default\noption.");

  conf.loadEnv();
  conf.loadCommandLine(ARGC, argv, false);
  conf.loadConfigFileIfGiven();

  EXPECT_EQ("text1",  conf.getParams()["test1"]);
  EXPECT_EQ("1",      conf.getParams()["test2"]);
  EXPECT_EQ("2010",   conf.getParams()["test3"]);
  EXPECT_EQ("",       conf.getParams()["test4"]);
}


TEST(ConfigurationTest, CommandLine)
{
  const int ARGC = 10;
  char* argv[ARGC];
  argv[0] = (char*) "./filename";
  argv[1] = (char*) "--test1=text1";
  argv[2] = (char*) "--test2";
  argv[3] = (char*) "-test3";
  argv[4] = (char*) "text2";
  argv[5] = (char*) "-test4";
  argv[6] = (char*) "-negative";
  argv[7] = (char*) "-25";
  argv[8] = (char*) "--cAsE=4";
  argv[9] = (char*) "--space=Long String";

  Configuration conf;
  conf.loadEnv();
  conf.loadCommandLine(ARGC, argv, false);
  conf.loadConfigFileIfGiven();

  EXPECT_EQ("text1",       conf.getParams()["test1"]);
  EXPECT_EQ("1",           conf.getParams()["test2"]);
  EXPECT_EQ("text2",       conf.getParams()["test3"]);
  EXPECT_EQ("1",           conf.getParams()["test4"]);
  EXPECT_EQ("-25",         conf.getParams()["negative"]);
  EXPECT_EQ("4",           conf.getParams()["case"]);
  EXPECT_EQ("Long String", conf.getParams()["space"]);
}


// Check whether specifying MESOS_HOME allows a config file to be loaded
// from the default config directory (MESOS_HOME/conf)
TEST_WITH_WORKDIR(ConfigurationTest, ConfigFileWithMesosHome)
{
  if (mkdir("conf", 0755) != 0)
    FAIL() << "Failed to create directory conf";
  ofstream file("conf/mesos.conf");
  file << "test1=coffee # beans are tasty\n";
  file << "# just a comment\n";
  file << "test2=tea\n";
  file.close();

  setenv("MESOS_HOME", ".", 1);
  Configuration conf;
  conf.loadEnv();
  conf.loadConfigFileIfGiven();
  unsetenv("MESOS_HOME");

  EXPECT_EQ("coffee", conf.getParams()["test1"]);
  EXPECT_EQ("tea",    conf.getParams()["test2"]);
}
  

// Check whether specifying just MESOS_CONF allows a config file to be loaded
TEST_WITH_WORKDIR(ConfigurationTest, ConfigFileWithConfDir)
{
  if (mkdir("conf2", 0755) != 0)
    FAIL() << "Failed to create directory conf2";
  ofstream file("conf2/mesos.conf");
  file << "test3=shake # sugar bomb\n";
  file << "# just a comment\n";
  file << "test4=milk\n";
  file.close();
  setenv("MESOS_CONF", "conf2", 1);
  Configuration conf;
  conf.loadEnv();
  conf.loadConfigFileIfGiven();
  unsetenv("MESOS_CONF");

  EXPECT_EQ("shake", conf.getParams()["test3"]);
  EXPECT_EQ("milk",  conf.getParams()["test4"]);
}


// Check that setting MESOS_CONF variable overrides the default location
// of conf directory relative in MESOS_HOME/conf.
TEST_WITH_WORKDIR(ConfigurationTest, ConfigFileWithHomeAndDir)
{
  if (mkdir("conf2", 0755) != 0)
    FAIL() << "Failed to create directory conf2";
  ofstream file("conf2/mesos.conf");
  file << "test3=shake # sugar bomb\n";
  file << "# just a comment\n";
  file << "test4=milk\n";
  file.close();
  setenv("MESOS_HOME", ".", 1);
  setenv("MESOS_CONF", "conf2", 1);
  Configuration conf;
  conf.loadEnv();
  conf.loadConfigFileIfGiven();
  unsetenv("MESOS_CONF");
  unsetenv("MESOS_HOME");

  EXPECT_EQ("shake", conf.getParams()["test3"]);
  EXPECT_EQ("milk",  conf.getParams()["test4"]);
}


// Check that when we specify a conf directory on the command line,
// we load values from the config file first and then the command line
TEST_WITH_WORKDIR(ConfigurationTest, CommandLineConfFlag)
{
  if (mkdir("conf2", 0755) != 0)
    FAIL() << "Failed to create directory conf2";
  ofstream file("conf2/mesos.conf");
  file << "a=1\n";
  file << "b=2\n";
  file << "c=3";
  file.close();

  const int ARGC = 4;
  char* argv[ARGC];
  argv[0] = (char*) "./filename";
  argv[1] = (char*) "--conf=conf2";
  argv[2] = (char*) "--b=overridden";
  argv[3] = (char*) "--d=fromCmdLine";

  Configuration conf;
  conf.loadEnv();
  conf.loadCommandLine(ARGC, argv, false);
  conf.loadConfigFileIfGiven();

  EXPECT_EQ("1",           conf.getParams()["a"]);
  EXPECT_EQ("overridden",  conf.getParams()["b"]);
  EXPECT_EQ("3",           conf.getParams()["c"]);
  EXPECT_EQ("fromCmdLine", conf.getParams()["d"]);
}


// Check that variables are loaded with the correct priority when an
// environment variable, a config file element , and a config flag
// are all present. Command line flags should have the highest priority,
// second should be environment variables, and last should be the file.
TEST_WITH_WORKDIR(ConfigurationTest, LoadingPriorities)
{
  // Create a file which contains parameters a, b, c and d
  if (mkdir("conf", 0755) != 0)
    FAIL() << "Failed to create directory conf";
  ofstream file("conf/mesos.conf");
  file << "a=fromFile\n";
  file << "b=fromFile\n";
  file << "c=fromFile\n";
  file << "d=fromFile\n";
  file.close();

  // Set environment to contain parameters a and b
  setenv("MESOS_HOME", ".", 1);
  setenv("MESOS_A", "fromEnv", 1);
  setenv("MESOS_B", "fromEnv", 1);

  // Pass parameters a and c from the command line
  const int ARGC = 3;
  char* argv[ARGC];
  argv[0] = (char*) "./filename";
  argv[1] = (char*) "--a=fromCmdLine";
  argv[2] = (char*) "--c=fromCmdLine";

  Configuration conf;
  conf.loadEnv();
  conf.loadCommandLine(ARGC, argv, false);
  conf.loadConfigFileIfGiven();

  // Clear the environment vars set above
  unsetenv("MESOS_HOME");
  unsetenv("MESOS_A");
  unsetenv("MESOS_B");

  // Check that every variable is obtained from the highest-priority location
  // (command line > env > file)
  EXPECT_EQ("fromCmdLine", conf.getParams()["a"]);
  EXPECT_EQ("fromEnv",     conf.getParams()["b"]);
  EXPECT_EQ("fromCmdLine", conf.getParams()["c"]);
  EXPECT_EQ("fromFile",    conf.getParams()["d"]);
}
