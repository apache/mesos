#include <glog/logging.h>
#include <gtest/gtest.h>

#include <libgen.h>
#include <stdlib.h>

#include <iostream>
#include <string>

#include "configurator.hpp"
#include "fatal.hpp"
#include "testing_utils.hpp"

using namespace mesos::internal;
using namespace mesos::internal::test;


int main(int argc, char **argv) {
  // Get absolute path to Mesos home direcotry (really src right now)
  char buf[4096];
  if (realpath(dirname(argv[0]), buf) == 0)
    fatalerror("Failed to find location of alltests using realpath");
  mesosHome = buf;

  // Clear any MESOS_ environment variables so they don't affect our tests
  Configurator::clearMesosEnvironmentVars();

  // Initialize Google Logging and Google Test
  google::InitGoogleLogging("alltests");
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  if (argc == 2 && strcmp("-v", argv[1]) == 0)
    google::SetStderrLogging(google::INFO);
  return RUN_ALL_TESTS();
}
