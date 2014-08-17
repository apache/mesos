#include <glog/logging.h>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <stout/glog.hpp>

int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  // Install default signal handler.
  // TODO(jieyu): We temporarily disable this since it causes some
  // flaky tests. Re-enable it once we find the root cause.
  // installFailureSignalHandler();

  return RUN_ALL_TESTS();
}
