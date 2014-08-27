#include <glog/logging.h>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  // Install GLOG's signal handler.
  google::InstallFailureSignalHandler();

  return RUN_ALL_TESTS();
}
