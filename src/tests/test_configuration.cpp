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
  Configuration c1;
  unsetenv("MESOS_TEST");

  EXPECT_EQ("working", c1.getParams()["test"]);
}


TEST(ConfigurationTest, CommandLine)
{
  const int ARGC = 6;
  char* argv[ARGC];
  argv[0] = (char*) "./filename";
  argv[1] = (char*) "--test1=text1";
  argv[2] = (char*) "--test2";
  argv[3] = (char*) "-test3";
  argv[4] = (char*) "text2";
  argv[5] = (char*) "-test4";

  Configuration c1(ARGC, argv, false);

  EXPECT_EQ("text1", c1.getParams()["test1"]);
  EXPECT_EQ("1", c1.getParams()["test2"]);
  EXPECT_EQ("text2", c1.getParams()["test3"]);
  EXPECT_EQ("1", c1.getParams()["test4"]);
}


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
  Configuration c1;
  unsetenv("MESOS_HOME");

  EXPECT_EQ("coffee", c1.getParams()["test1"]);
  EXPECT_EQ("tea", c1.getParams()["test2"]);
}
  

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
  unsetenv("MESOS_CONF");

  EXPECT_EQ("shake", conf.getParams()["test3"]);
  EXPECT_EQ("milk", conf.getParams()["test4"]);
}


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
  unsetenv("MESOS_CONF");
  unsetenv("MESOS_HOME");

  EXPECT_EQ("shake", conf.getParams()["test3"]);
  EXPECT_EQ("milk", conf.getParams()["test4"]);
}


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

  Configuration c1(ARGC, argv, false);

  EXPECT_EQ("1", c1.getParams()["a"]);
  EXPECT_EQ("overridden", c1.getParams()["b"]);
  EXPECT_EQ("3", c1.getParams()["c"]);
  EXPECT_EQ("fromCmdLine", c1.getParams()["d"]);
}
