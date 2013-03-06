#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <process/process.hpp>

int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  // Initialize libprocess.
  process::initialize();

  return RUN_ALL_TESTS();
}
