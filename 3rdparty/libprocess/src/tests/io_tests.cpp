#include <gmock/gmock.h>

#include <string>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/io.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>

#include "encoder.hpp"

using namespace process;

using std::string;


TEST(IO, Poll)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  int pipes[2];
  pipe(pipes);

  Future<short> future = io::poll(pipes[0], io::READ);

  EXPECT_FALSE(future.isReady());

  ASSERT_EQ(3, write(pipes[1], "hi", 3));

  AWAIT_EXPECT_EQ(io::READ, future);

  close(pipes[0]);
  close(pipes[1]);
}


TEST(IO, Read)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  int pipes[2];
  char data[3];

  // Create a blocking pipe.
  ASSERT_NE(-1, ::pipe(pipes));

  // Test on a blocking file descriptor.
  AWAIT_EXPECT_FAILED(io::read(pipes[0], data, 3));

  close(pipes[0]);
  close(pipes[1]);

  // Test on a closed file descriptor.
  AWAIT_EXPECT_FAILED(io::read(pipes[0], data, 3));

  // Create a nonblocking pipe.
  ASSERT_NE(-1, ::pipe(pipes));
  ASSERT_SOME(os::nonblock(pipes[0]));
  ASSERT_SOME(os::nonblock(pipes[1]));

  // Test reading nothing.
  AWAIT_EXPECT_FAILED(io::read(pipes[0], data, 0));

  // Test successful read.
  Future<size_t> future = io::read(pipes[0], data, 3);
  ASSERT_FALSE(future.isReady());

  ASSERT_EQ(2, write(pipes[1], "hi", 2));

  AWAIT_ASSERT_EQ(2u, future);
  EXPECT_EQ('h', data[0]);
  EXPECT_EQ('i', data[1]);

  // Test cancellation.
  future = io::read(pipes[0], data, 1);
  ASSERT_FALSE(future.isReady());

  future.discard();

  ASSERT_EQ(3, write(pipes[1], "omg", 3));

  AWAIT_ASSERT_EQ(3u, io::read(pipes[0], data, 3));
  EXPECT_EQ('o', data[0]);
  EXPECT_EQ('m', data[1]);
  EXPECT_EQ('g', data[2]);

  // Test read EOF.
  future = io::read(pipes[0], data, 3);
  ASSERT_FALSE(future.isReady());

  close(pipes[1]);

  AWAIT_ASSERT_EQ(0u, future);

  close(pipes[0]);
}


TEST(IO, BufferedRead)
{
  // 128 Bytes.
  string data =
      "This data is much larger than BUFFERED_READ_SIZE, which means it will "
      "trigger multiple buffered async reads as a result.........";
  ASSERT_EQ(128u, data.size());

  // Keep doubling the data size until we're guaranteed to trigger at least
  // 3 buffered async reads.
  while (data.length() < 3 * io::BUFFERED_READ_SIZE) {
    data.append(data);
  }

  // First read from a file.
  ASSERT_SOME(os::write("file", data));

  Try<int> fd = os::open("file", O_RDONLY);
  ASSERT_SOME(fd);

  // Read from blocking fd.
  AWAIT_EXPECT_FAILED(io::read(fd.get()));

  // Read from non-blocking fd.
  ASSERT_TRUE(os::nonblock(fd.get()).isSome());
  AWAIT_EXPECT_EQ(data, io::read(fd.get()));

  os::close(fd.get());

  // Now read from pipes.
  int pipes[2];

  // Create a blocking pipe.
  ASSERT_NE(-1, ::pipe(pipes));

  // Test on a blocking pipe.
  AWAIT_EXPECT_FAILED(io::read(pipes[0]));

  close(pipes[0]);
  close(pipes[1]);

  // Test on a closed pipe.
  AWAIT_EXPECT_FAILED(io::read(pipes[0]));

  // Create a nonblocking pipe for reading.
  ASSERT_NE(-1, ::pipe(pipes));
  ASSERT_SOME(os::nonblock(pipes[0]));

  // Test a successful read from the pipe.
  Future<string> future = io::read(pipes[0]);

  // At first, the future will not be ready until we write to and
  // close the pipe.
  ASSERT_FALSE(future.isReady());

  ASSERT_SOME(os::write(pipes[1], data));
  close(pipes[1]);

  AWAIT_EXPECT_EQ(data, future);

  close(pipes[0]);
}
