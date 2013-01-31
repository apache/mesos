#include <gmock/gmock.h>

#include <string>

#include <process/future.hpp>
#include <process/io.hpp>

#include <stout/os.hpp>

#include "encoder.hpp"

using namespace process;


TEST(IO, Poll)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  int pipes[2];
  pipe(pipes);

  Future<short> future = io::poll(pipes[0], io::READ);

  EXPECT_FALSE(future.isReady());

  ASSERT_EQ(3, write(pipes[1], "hi", 3));

  future.await();

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ(io::READ, future.get());

  close(pipes[0]);
  close(pipes[1]);
}


TEST(IO, Read)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  int pipes[2];
  char data[3];
  Future<size_t> future;

  // Create a blocking pipe.
  ASSERT_NE(-1, ::pipe(pipes));

  // Test on a blocking file descriptor.
  future = io::read(pipes[0], data, 3);
  future.await(Seconds(1.0));
  EXPECT_TRUE(future.isFailed());

  close(pipes[0]);
  close(pipes[1]);

  // Test on a closed file descriptor.
  future = io::read(pipes[0], data, 3);
  future.await(Seconds(1.0));
  EXPECT_TRUE(future.isFailed());

  // Create a nonblocking pipe.
  ASSERT_NE(-1, ::pipe(pipes));
  ASSERT_TRUE(os::nonblock(pipes[0]).isSome());
  ASSERT_TRUE(os::nonblock(pipes[1]).isSome());

  // Test reading nothing.
  future = io::read(pipes[0], data, 0);
  future.await(Seconds(1.0));
  EXPECT_TRUE(future.isFailed());

  // Test successful read.
  future = io::read(pipes[0], data, 3);
  ASSERT_FALSE(future.isReady());

  ASSERT_EQ(2, write(pipes[1], "hi", 2));
  future.await(Seconds(1.0));
  ASSERT_TRUE(future.isReady());
  ASSERT_EQ(2, future.get());
  EXPECT_EQ('h', data[0]);
  EXPECT_EQ('i', data[1]);

  // Test cancellation.
  future = io::read(pipes[0], data, 1);
  ASSERT_FALSE(future.isReady());

  future.discard();

  ASSERT_EQ(3, write(pipes[1], "omg", 3));

  future = io::read(pipes[0], data, 3);
  future.await(Seconds(1.0));
  ASSERT_TRUE(future.isReady());
  ASSERT_EQ(3, future.get());
  EXPECT_EQ('o', data[0]);
  EXPECT_EQ('m', data[1]);
  EXPECT_EQ('g', data[2]);

  // Test read EOF.
  future = io::read(pipes[0], data, 3);
  ASSERT_FALSE(future.isReady());

  close(pipes[1]);

  future.await(Seconds(1.0));
  ASSERT_TRUE(future.isReady());
  EXPECT_EQ(0, future.get());

  close(pipes[0]);
}


TEST(IO, BufferedRead)
{
  // 128 Bytes.
  std::string data =
      "This data is much larger than BUFFERED_READ_SIZE, which means it will "
      "trigger multiple buffered async reads as a result.........";
  CHECK(data.size() == 128);

  // Keep doubling the data size until we're guaranteed to trigger at least
  // 3 buffered async reads.
  while (data.length() < 3 * io::BUFFERED_READ_SIZE) {
    data.append(data);
  }

  ASSERT_TRUE(os::write("file", data).isSome());

  Try<int> fd = os::open("file", O_RDONLY);
  ASSERT_TRUE(fd.isSome());

  // Read from blocking fd.
  Future<std::string> future = io::read(fd.get());
  ASSERT_TRUE(future.await(Seconds(5.0)));
  EXPECT_TRUE(future.isFailed());

  // Read from non-blocking fd.
  ASSERT_TRUE(os::nonblock(fd.get()).isSome());

  future = io::read(fd.get());
  ASSERT_TRUE(future.await(Seconds(5.0)));
  EXPECT_TRUE(future.isReady());
  EXPECT_EQ(data, future.get());

  os::close(fd.get());
}
