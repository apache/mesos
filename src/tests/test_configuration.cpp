#include <gtest/gtest.h>
#include <iostream>
#include <fstream>

#include <boost/lexical_cast.hpp>

#include "configuration.hpp"

using std::ofstream;
using std::string;
using std::vector;
using std::cout;

using boost::lexical_cast;

using namespace nexus;
using namespace nexus::internal;

TEST(ConfigurationTest, EnvironTest)
{

  setenv("MESOS_TEST","working", true);
  Configuration c1;
  unsetenv("MESOS_TEST");

  EXPECT_TRUE(c1.getParams()["TEST"] == "working");
}

TEST(ConfigurationTest, CmdTest)
{
  #define ARGC 6
  char *argv[ARGC];
  argv[0] = (char *) "./filename";
  argv[1] = (char *) "--test1=text1";
  argv[2] = (char *) "--test2";
  argv[3] = (char *) "-test3";
  argv[4] = (char *) "text2";
  argv[5] = (char *) "-test4";

  Configuration c1(ARGC, argv);

  EXPECT_TRUE(c1.getParams()["test1"] == "text1");
  EXPECT_TRUE(c1.getParams()["test2"] == "1");
  EXPECT_TRUE(c1.getParams()["test3"] == "text2");
  EXPECT_TRUE(c1.getParams()["test4"] == "1");
}

// The below test creates files. We have to enable tests to do that safely.
/*
TEST(ConfigurationTest, ConfTest)
{
  ofstream file("mesos.conf");
  file << "TEST1=coffee # beans are tasty\n";
  file << "# just a comment\n";
  file << "TEST2=tea\n";
  file.close();

  setenv("MESOS_HOME", "./", 1);
  Configuration c1;
  unsetenv("MESOS_HOME");

  EXPECT_TRUE(c1.getParams()["TEST1"] == "coffee");
  EXPECT_TRUE(c1.getParams()["TEST2"] == "tea");

  ofstream file2("misus.conf");
  file2 << "TEST3=shake # sugar bomb\n";
  file2 << "# just a comment\n";
  file2 << "TEST4=milk\n";
  file2.close();
  setenv("MESOS_CONF", "misus.conf", 1);
  Configuration c2;
  unsetenv("MESOS_CONF");

  EXPECT_TRUE(c2.getParams()["TEST3"] == "shake");
  EXPECT_TRUE(c2.getParams()["TEST4"] == "milk");
}
*/
