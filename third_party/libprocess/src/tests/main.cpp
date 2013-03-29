#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/process.hpp>

int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  // Initialize libprocess.
  process::initialize();

  // Add the libprocess test event listeners.
  ::testing::TestEventListeners& listeners =
    ::testing::UnitTest::GetInstance()->listeners();

  listeners.Append(process::ClockTestEventListener::instance());
  listeners.Append(process::FilterTestEventListener::instance());

  return RUN_ALL_TESTS();
}
