#include <glog/logging.h>
#include <gtest/gtest.h>


int main(int argc, char **argv) {
  google::InitGoogleLogging("alltests");
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  if (argc == 2 && strcmp("-v", argv[1]) == 0)
    google::SetStderrLogging(google::INFO);
  return RUN_ALL_TESTS();
}
