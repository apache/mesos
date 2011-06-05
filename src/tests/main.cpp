#include <glog/logging.h>
#include <gtest/gtest.h>

#include <libgen.h>
#include <stdlib.h>

#include <iostream>
#include <string>

using std::string;

string MESOS_HOME;


int main(int argc, char **argv) {
  // Get absolute path to Mesos home direcotry (really src right now)
  char buf[4096];
  realpath(dirname(argv[0]), buf);
  MESOS_HOME = buf;
  std::cout << "Mesos home is " << MESOS_HOME << std::endl;

  google::InitGoogleLogging("alltests");
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  if (argc == 2 && strcmp("-v", argv[1]) == 0)
    google::SetStderrLogging(google::INFO);
  return RUN_ALL_TESTS();
}
