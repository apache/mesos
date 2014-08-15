#include <glog/logging.h>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <stout/glog.hpp>

int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  // Install default signal handler.
  installFailureSignalHandler();

  return RUN_ALL_TESTS();
}
