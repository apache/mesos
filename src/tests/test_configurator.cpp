#include <gtest/gtest.h>

#include <iostream>
#include <fstream>

#include <boost/lexical_cast.hpp>

#include "testing_utils.hpp"

#include "configurator/configurator.hpp"

using std::ofstream;
using std::string;
using std::vector;
using std::cout;

using boost::lexical_cast;

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;


TEST(ConfiguratorTest, Environment)
{
  setenv("MESOS_TEST", "working", true);
  Configurator conf;
  conf.load();
  unsetenv("MESOS_TEST");

  EXPECT_EQ("working", conf.getParams()["test"]);
}


TEST(ConfiguratorTest, DefaultOptions)
{
  const int ARGC = 4;
  char* argv[ARGC];
  argv[0] = (char*) "bin/filename";
  argv[1] = (char*) "--test1=501";
  argv[2] = (char*) "--test2";
  argv[3] = (char*) "--excp=txt";

  Configurator conf;

  EXPECT_NO_THROW(conf.addOption<int>("test1", "Testing option", 500));
  EXPECT_NO_THROW(conf.addOption<bool>("test2", "Another tester", 0));
  EXPECT_NO_THROW(conf.addOption<long>("test3", "Tests the default", 2010));
  EXPECT_NO_THROW(conf.addOption<string>("test4", "Option without default"));
  EXPECT_NO_THROW(conf.addOption<string>("test5", "Option with a default", 
                                         "arb"));
  EXPECT_NO_THROW(conf.addOption<bool>("test6", "Toggler...", false));
  EXPECT_NO_THROW(conf.addOption<bool>("test7", "Toggler...", true));
  EXPECT_NO_THROW(conf.load(ARGC, argv, false));

  EXPECT_NO_THROW(conf.addOption<int>("excp", "Exception tester.", 50));
  EXPECT_THROW(conf.validate(), ConfigurationException);
  conf.getParams()["excp"] = "27";
  EXPECT_NO_THROW(conf.validate());

  EXPECT_EQ("501",    conf.getParams()["test1"]);
  EXPECT_EQ("1",      conf.getParams()["test2"]);
  EXPECT_EQ("2010",   conf.getParams()["test3"]);
  EXPECT_EQ("",       conf.getParams()["test4"]);
  EXPECT_EQ("arb",    conf.getParams()["test5"]);
  EXPECT_EQ("27",     conf.getParams()["excp"]);
  EXPECT_EQ("0",      conf.getParams()["test6"]);
  EXPECT_EQ("1",      conf.getParams()["test7"]);
}


TEST(ConfiguratorTest, CommandLine)
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

  EXPECT_NO_THROW( conf.load(ARGC, argv, false) );

  EXPECT_EQ("text1",       conf.getParams()["test1"]);
  EXPECT_EQ("1",           conf.getParams()["test2"]);
  EXPECT_EQ("-25",         conf.getParams()["negative"]);
  EXPECT_EQ("4",           conf.getParams()["case"]);
  EXPECT_EQ("Long String", conf.getParams()["space"]);
  EXPECT_EQ("1",           conf.getParams()["bool1"]);
  EXPECT_EQ("0",           conf.getParams()["bool2"]);
  EXPECT_EQ("1",           conf.getParams()["bool3"]);
  EXPECT_EQ("0",           conf.getParams()["bool4"]);
}


// Check whether specifying MESOS_HOME allows a config file to be loaded
// from the default config directory (MESOS_HOME/conf)
TEST_WITH_WORKDIR(ConfiguratorTest, ConfigFileWithMesosHome)
{
  if (mkdir("bin", 0755) != 0)
    FAIL() << "Failed to create directory bin";
  if (mkdir("conf", 0755) != 0)
    FAIL() << "Failed to create directory conf";
  ofstream file("conf/mesos.conf");
  file << "test1=coffee # beans are tasty\n";
  file << "# just a comment\n";
  file << "test2=tea\n";
  file.close();

  setenv("MESOS_HOME", ".", 1);
  Configurator conf;
  char* argv[1] = { (char*) "bin/foo" };
  EXPECT_NO_THROW( conf.load(1, argv, true) );
  unsetenv("MESOS_HOME");

  EXPECT_EQ("coffee", conf.getParams()["test1"]);
  EXPECT_EQ("tea",    conf.getParams()["test2"]);
}
  

// Check whether specifying just MESOS_CONF allows a config file to be loaded
TEST_WITH_WORKDIR(ConfiguratorTest, ConfigFileWithConfDir)
{
  if (mkdir("conf2", 0755) != 0)
    FAIL() << "Failed to create directory conf2";
  ofstream file("conf2/mesos.conf");
  file << "test3=shake # sugar bomb\n";
  file << "# just a comment\n";
  file << "test4=milk\n";
  file.close();
  setenv("MESOS_CONF", "conf2", 1);
  Configurator conf;
  EXPECT_NO_THROW( conf.load() );
  unsetenv("MESOS_CONF");

  EXPECT_EQ("shake", conf.getParams()["test3"]);
  EXPECT_EQ("milk",  conf.getParams()["test4"]);
}


// Check that setting MESOS_CONF variable overrides the default location
// of conf directory relative in MESOS_HOME/conf.
TEST_WITH_WORKDIR(ConfiguratorTest, ConfigFileWithHomeAndDir)
{
  if (mkdir("bin", 0755) != 0)
    FAIL() << "Failed to create directory bin";
  if (mkdir("conf2", 0755) != 0)
    FAIL() << "Failed to create directory conf2";
  ofstream file("conf2/mesos.conf");
  file << "test3=shake # sugar bomb\n";
  file << "# just a comment\n";
  file << "test4=milk\n";
  file.close();

  setenv("MESOS_CONF", "conf2", 1);
  Configurator conf;
  char* argv[1] = { (char*) "bin/foo" };
  EXPECT_NO_THROW( conf.load(1, argv, true) );
  unsetenv("MESOS_CONF");

  EXPECT_EQ("shake", conf.getParams()["test3"]);
  EXPECT_EQ("milk",  conf.getParams()["test4"]);
}


// Check that when we specify a conf directory on the command line,
// we load values from the config file first and then the command line
TEST_WITH_WORKDIR(ConfiguratorTest, CommandLineConfFlag)
{
  if (mkdir("bin", 0755) != 0)
    FAIL() << "Failed to create directory bin";
  if (mkdir("conf2", 0755) != 0)
    FAIL() << "Failed to create directory conf2";
  ofstream file("conf2/mesos.conf");
  file << "a=1\n";
  file << "b=2\n";
  file << "c=3";
  file.close();

  const int ARGC = 4;
  char* argv[ARGC];
  argv[0] = (char*) "bin/filename";
  argv[1] = (char*) "--conf=conf2";
  argv[2] = (char*) "--b=overridden";
  argv[3] = (char*) "--d=fromCmdLine";

  Configurator conf;
  EXPECT_NO_THROW( conf.load(ARGC, argv, false) );

  EXPECT_EQ("1",           conf.getParams()["a"]);
  EXPECT_EQ("overridden",  conf.getParams()["b"]);
  EXPECT_EQ("3",           conf.getParams()["c"]);
  EXPECT_EQ("fromCmdLine", conf.getParams()["d"]);
}


// Check that variables are loaded with the correct priority when an
// environment variable, a config file element , and a config flag
// are all present. Command line flags should have the highest priority,
// second should be environment variables, and last should be the file.
TEST_WITH_WORKDIR(ConfiguratorTest, LoadingPriorities)
{
  // Create a file which contains parameters a, b, c and d
  if (mkdir("bin", 0755) != 0)
    FAIL() << "Failed to create directory bin";
  if (mkdir("conf", 0755) != 0)
    FAIL() << "Failed to create directory conf";
  ofstream file("conf/mesos.conf");
  file << "a=fromFile\n";
  file << "b=fromFile\n";
  file << "c=fromFile\n";
  file << "d=fromFile\n";
  file.close();

  // Set environment to contain parameters a and b
  setenv("MESOS_A", "fromEnv", 1);
  setenv("MESOS_B", "fromEnv", 1);

  // Pass parameters a and c from the command line
  const int ARGC = 3;
  char* argv[ARGC];
  argv[0] = (char*) "bin/filename";
  argv[1] = (char*) "--a=fromCmdLine";
  argv[2] = (char*) "--c=fromCmdLine";

  Configurator conf;
  EXPECT_NO_THROW( conf.load(ARGC, argv, true) );

  // Clear the environment vars set above
  unsetenv("MESOS_A");
  unsetenv("MESOS_B");

  // Check that every variable is obtained from the highest-priority location
  // (command line > env > file)
  EXPECT_EQ("fromCmdLine", conf.getParams()["a"]);
  EXPECT_EQ("fromEnv",     conf.getParams()["b"]);
  EXPECT_EQ("fromCmdLine", conf.getParams()["c"]);
  EXPECT_EQ("fromFile",    conf.getParams()["d"]);
}


// Check that spaces before and after the = signs in config files are ignored
TEST_WITH_WORKDIR(ConfiguratorTest, ConfigFileSpacesIgnored)
{
  if (mkdir("conf", 0755) != 0)
    FAIL() << "Failed to create directory conf";
  ofstream file("conf/mesos.conf");
  file << "test1=coffee # beans are tasty\n";
  file << "# just a comment\n";
  file << "  \t # comment with spaces in front\n";
  file << "\n";
  file << "test2 =tea\n";
  file << "test3=  water\n";
  file << "   test4 =  milk\n";
  file << "  test5 =  hot  chocolate\t\n";
  file << "\ttest6 =  juice# #\n";
  file.close();

  Configurator conf;
  setenv("MESOS_CONF", "conf", 1);
  EXPECT_NO_THROW(conf.load());
  unsetenv("MESOS_CONF");

  EXPECT_EQ("coffee",         conf.getParams()["test1"]);
  EXPECT_EQ("tea",            conf.getParams()["test2"]);
  EXPECT_EQ("water",          conf.getParams()["test3"]);
  EXPECT_EQ("milk",           conf.getParams()["test4"]);
  EXPECT_EQ("hot  chocolate", conf.getParams()["test5"]);
  EXPECT_EQ("juice",          conf.getParams()["test6"]);
}


// Check that exceptions are thrown on invalid config file
TEST_WITH_WORKDIR(ConfiguratorTest, MalformedConfigFile)
{
  if (mkdir("conf", 0755) != 0)
    FAIL() << "Failed to create directory conf";
  ofstream file("conf/mesos.conf");
  file << "test1=coffee\n";
  file << "JUNK\n";
  file << "test2=tea\n";
  file.close();

  setenv("MESOS_CONF", "conf", 1);
  Configurator conf;
  EXPECT_THROW(conf.load(), ConfigurationException);
  unsetenv("MESOS_CONF");
}
