#include <errno.h>

#include <string>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/os.hpp>

using std::string;

// TODO(bmahler): Expose OsTest so this can use it.
class OsSignalsTest : public ::testing::Test {};


TEST_F(OsSignalsTest, Suppress)
{
  int pipes[2];

  ASSERT_NE(-1, pipe(pipes));

  ASSERT_SOME(os::close(pipes[0]));

  const string data = "hello";

  // Let's make sure we can suppress SIGPIPE!
  suppress(SIGPIPE) {
    // Writing to a pipe that has been closed generates SIGPIPE.
    ASSERT_EQ(-1, write(pipes[1], data.c_str(), data.length()));

    ASSERT_EQ(EPIPE, errno);
  }

  ASSERT_SOME(os::close(pipes[1]));
}
