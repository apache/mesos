#include <gtest/gtest.h>

#include <gmock/gmock.h>

int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  return RUN_ALL_TESTS();
}
