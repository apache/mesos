#include <glog/logging.h>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  // Handles SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGBUS, SIGTERM
  // by default.
  google::InstallFailureSignalHandler();

  return RUN_ALL_TESTS();
}
