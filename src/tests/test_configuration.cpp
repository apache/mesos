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

  EXPECT_TRUE(c1.getParams()["test"] == "working");
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

  EXPECT_TRUE(c1.getParams()["test1"] == "text1");
  EXPECT_TRUE(c1.getParams()["test2"] == "1");
  EXPECT_TRUE(c1.getParams()["test3"] == "text2");
  EXPECT_TRUE(c1.getParams()["test4"] == "1");
}


TEST(ConfigurationTest, ConfigFile)
{
  enterTestDirectory("ConfigurationTest", "ConfigFile");

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

  EXPECT_TRUE(c1.getParams()["test1"] == "coffee");
  EXPECT_TRUE(c1.getParams()["test2"] == "tea");

  if (mkdir("conf2", 0755) != 0)
    FAIL() << "Failed to create directory conf2";
  ofstream file2("conf2/mesos.conf");
  file2 << "test3=shake # sugar bomb\n";
  file2 << "# just a comment\n";
  file2 << "test4=milk\n";
  file2.close();
  setenv("MESOS_CONF", "conf2", 1);
  Configuration c2;
  unsetenv("MESOS_CONF");

  EXPECT_TRUE(c2.getParams()["test3"] == "shake");
  EXPECT_TRUE(c2.getParams()["test4"] == "milk");
}
