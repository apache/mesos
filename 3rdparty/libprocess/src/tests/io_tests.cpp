// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <gmock/gmock.h>

#include <string>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/io.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>

#include <stout/os/pipe.hpp>
#include <stout/os/read.hpp>
#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

#include "encoder.hpp"

namespace io = process::io;

using process::Clock;
using process::Future;

using std::array;
using std::string;

class IOTest: public TemporaryDirectoryTest {};


TEST_F(IOTest, PollRead)
{
  Try<std::array<int_fd, 2>> pipes = os::pipe();
  ASSERT_SOME(pipes);
  ASSERT_SOME(io::prepare_async((*pipes)[0]));
  ASSERT_SOME(io::prepare_async((*pipes)[1]));

  // Test discard when polling.
  Future<short> future = io::poll((*pipes)[0], io::READ);
  EXPECT_TRUE(future.isPending());
  future.discard();
  AWAIT_DISCARDED(future);

  // Test successful polling.
  future = io::poll((*pipes)[0], io::READ);

  // This is not sufficient for ensuring the event loop does
  // not detect readiness, since it's happening asynchronously
  // in the kernel.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_TRUE(future.isPending());

  ASSERT_EQ(2, write((*pipes)[1], "hi", 2));
  AWAIT_EXPECT_EQ(io::READ, future);

  ASSERT_SOME(os::close((*pipes)[0]));
  ASSERT_SOME(os::close((*pipes)[1]));
}


// We do not support write readiness polling on Windows
// at the current time.
#ifndef __WINDOWS__
TEST_F(IOTest, PollWrite)
{
  int pipes[2];
  ASSERT_NE(-1, pipe(pipes));
  ASSERT_SOME(io::prepare_async(pipes[0]));
  ASSERT_SOME(io::prepare_async(pipes[1]));

  // Fill up the pipe to test write readiness polling.
  while (true) {
    int result = ::write(pipes[1], "hello world!", sizeof("hello world!"));

    if (result < 0) {
      if (errno == EAGAIN) {
        break;
      }
      FAIL() << "Unexpected error: " << strerror(errno);
    }
  }

  // Test discard when polling.
  Future<short> future = io::poll(pipes[1], io::WRITE);
  EXPECT_TRUE(future.isPending());
  future.discard();
  AWAIT_DISCARDED(future);

  // Test successful polling.
  future = io::poll(pipes[1], io::WRITE);

  // This is not sufficient for ensuring the event loop does
  // not detect readiness, since it's happening asynchronously
  // in the kernel.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_TRUE(future.isPending());

  // It appears that Linux does not notify of write readiness
  // until the reader reads at least a page of data.
  size_t size = os::pagesize();
  std::unique_ptr<char[]> buffer(new char[size]);
  ASSERT_EQ(size, ::read(pipes[0], buffer.get(), size));

  AWAIT_EXPECT_EQ(io::WRITE, future);

  ASSERT_SOME(os::close(pipes[0]));
  ASSERT_SOME(os::close(pipes[1]));
}
#endif // __WINDOWS__


TEST_F(IOTest, Read)
{
  char data[3];

  // Create a blocking pipe.
  Try<array<int_fd, 2>> pipes_ = os::pipe();
  ASSERT_SOME(pipes_);

  array<int_fd, 2> pipes = pipes_.get();

  // Test on a blocking file descriptor.
  AWAIT_EXPECT_FAILED(io::read(pipes[0], data, 3));

  ASSERT_SOME(os::close(pipes[0]));
  ASSERT_SOME(os::close(pipes[1]));

  // Test on a closed file descriptor.
  AWAIT_EXPECT_FAILED(io::read(pipes[0], data, 3));

  // Create a nonblocking pipe.
  pipes_ = os::pipe();
  ASSERT_SOME(pipes_);
  pipes = pipes_.get();

  ASSERT_SOME(io::prepare_async(pipes[0]));
  ASSERT_SOME(io::prepare_async(pipes[1]));

  // Test reading nothing.
  AWAIT_EXPECT_EQ(0u, io::read(pipes[0], data, 0));

  // Test discarded read.
  Future<size_t> future = io::read(pipes[0], data, 3);
  EXPECT_TRUE(future.isPending());
  future.discard();
  AWAIT_DISCARDED(future);

  // Test successful read.
  future = io::read(pipes[0], data, 3);
  ASSERT_FALSE(future.isReady());

  ASSERT_EQ(2, os::write(pipes[1], "hi", 2));

  AWAIT_ASSERT_EQ(2u, future);
  EXPECT_EQ('h', data[0]);
  EXPECT_EQ('i', data[1]);

  // Test cancellation.
  future = io::read(pipes[0], data, 1);
  ASSERT_FALSE(future.isReady());

  future.discard();

  future = io::read(pipes[0], data, 3);
  ASSERT_EQ(3, os::write(pipes[1], "omg", 3));

  AWAIT_ASSERT_EQ(3u, future) << string(data, 2);
  EXPECT_EQ('o', data[0]);
  EXPECT_EQ('m', data[1]);
  EXPECT_EQ('g', data[2]);

  // Test read EOF.
  future = io::read(pipes[0], data, 3);
  ASSERT_FALSE(future.isReady());

  ASSERT_SOME(os::close(pipes[1]));

  AWAIT_ASSERT_EQ(0u, future);

  ASSERT_SOME(os::close(pipes[0]));
}


TEST_F(IOTest, BufferedRead)
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

  Try<int_fd> fd = os::open("file", O_RDONLY | O_CLOEXEC);
  ASSERT_SOME(fd);

  AWAIT_EXPECT_EQ(data, io::read(fd.get()));

  ASSERT_SOME(os::close(fd.get()));

  // Now read from pipes.
  Try<array<int_fd, 2>> pipes_ = os::pipe();
  ASSERT_SOME(pipes_);

  // Test on a closed pipe.
  array<int_fd, 2> pipes = pipes_.get();
  ASSERT_SOME(os::close(pipes[0]));
  ASSERT_SOME(os::close(pipes[1]));

  AWAIT_EXPECT_FAILED(io::read(pipes[0]));

  // Test a successful read from the pipe.
  pipes_ = os::pipe();
  ASSERT_SOME(pipes_);
  pipes = pipes_.get();

  // At first, the future will not be ready until we write to and
  // close the pipe.
  Future<string> future = io::read(pipes[0]);
  ASSERT_FALSE(future.isReady());

  ASSERT_SOME(os::write(pipes[1], data));
  ASSERT_SOME(os::close(pipes[1]));

  AWAIT_EXPECT_EQ(data, future);

  ASSERT_SOME(os::close(pipes[0]));

  ASSERT_SOME(os::rm("file"));
}


TEST_F(IOTest, Write)
{
  // Create a blocking pipe.
  Try<array<int_fd, 2>> pipes_ = os::pipe();
  ASSERT_SOME(pipes_);
  array<int_fd, 2> pipes = pipes_.get();

  // Test on a blocking file descriptor.
  AWAIT_EXPECT_FAILED(io::write(pipes[1], (void*) "hi", 2));

  ASSERT_SOME(os::close(pipes[0]));
  ASSERT_SOME(os::close(pipes[1]));

  // Test on a closed file descriptor.
  AWAIT_EXPECT_FAILED(io::write(pipes[1], (void*) "hi", 2));

  // Create a nonblocking pipe.
  pipes_ = os::pipe();
  ASSERT_SOME(pipes_);
  pipes = pipes_.get();

  ASSERT_SOME(io::prepare_async(pipes[0]));
  ASSERT_SOME(io::prepare_async(pipes[1]));

  // Test writing nothing.
  AWAIT_EXPECT_EQ(0u, io::write(pipes[1], (void*) "hi", 0));

  // Test successful write.
  Future<size_t> future = io::write(pipes[1], (void*) "hi", 2);

  char data[2];
  AWAIT_EXPECT_EQ(2u, io::read(pipes[0], data, 2));
  EXPECT_EQ("hi", string(data, 2));

  // Test write to broken pipe.
  ASSERT_SOME(os::close(pipes[0]));
  AWAIT_EXPECT_FAILED(io::write(pipes[1], (void*) "hi", 2));

  ASSERT_SOME(os::close(pipes[1]));
}


#ifdef __WINDOWS__
TEST_F(IOTest, BlockingWrite)
#else
// TODO(alexr): Enable after MESOS-973 is resolved.
TEST_F(IOTest, DISABLED_BlockingWrite)
#endif // __WINDOWS__
{
  Try<array<int_fd, 2>> pipes_ = os::pipe();
  ASSERT_SOME(pipes_);

  array<int_fd, 2> pipes = pipes_.get();

  // Get the pipe buffer size. On Windows, we can query this directly. On
  // other platforms, we do non-blocking writes until we get `EAGAIN` or
  // `EWOULDBLOCK`.
#ifdef __WINDOWS__
  DWORD outBufferSize;
  const BOOL success = ::GetNamedPipeInfo(
      pipes[0],       // Pipe `HANDLE`.
      nullptr,        // Flags.
      &outBufferSize, // Outbound (write) buffer size.
      nullptr,        // Inbound (read) buffer size.
      nullptr);       // Max instances of the named pipe.

  ASSERT_TRUE(success);

  size_t size = static_cast<size_t>(outBufferSize);
  if (size < 4096) {
    // On Windows, the buffer size can be very small and even 0, which will
    // break this test, so we report that the buffer size is bigger if it's
    // too small. This doesn't change the test semantics; all it does is
    // make the test write a longer string.
    size = 4096;
  }
#else
  // Make pipes non-blocking.
  ASSERT_SOME(io::prepare_async(pipes[0]));
  ASSERT_SOME(io::prepare_async(pipes[1]));

  // Determine the pipe buffer size by writing until we block.
  size_t size = 0;
  ssize_t written = 0;
  while ((written = ::write(pipes[1], "data", 4)) >= 0) {
    size += written;
  }

  ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

  ASSERT_SOME(os::close(pipes[0]));
  ASSERT_SOME(os::close(pipes[1]));

  // Recreate a nonblocking pipe.
  pipes_ = os::pipe();
  ASSERT_SOME(pipes_);
  pipes = pipes_.get();
#endif // __WINDOWS__

  ASSERT_SOME(io::prepare_async(pipes[0]));
  ASSERT_SOME(io::prepare_async(pipes[1]));

  // Create 8 pipe buffers worth of data. Try and write all the data
  // at once. Check that the future is pending after doing the
  // write. Then read 128 bytes and make sure the write remains
  // pending.

  string data = "data"; // 4 Bytes.
  ASSERT_EQ(4u, data.size());

  while (data.size() < (8 * size)) {
    data.append(data);
  }

  Future<Nothing> future1 = io::write(pipes[1], data);

  EXPECT_TRUE(future1.isPending());

  // Check that a subsequent write remains pending and can be
  // discarded.
  Future<Nothing> future2 = io::write(pipes[1], "hello world");
  EXPECT_TRUE(future2.isPending());
  future2.discard();
  AWAIT_DISCARDED(future2);

  // Check after reading some data the first write remains pending.
  ASSERT_LT(128u, size);
  char temp[128];
  AWAIT_EXPECT_EQ(128u, io::read(pipes[0], temp, 128));

  EXPECT_TRUE(future1.isPending());

  // Now read all the data we wrote the first time and expect the
  // first future to succeed since the second future should have been
  // completely discarded.
  ssize_t length = 128; // To account for io::read above.
  while (length < static_cast<ssize_t>(data.size())) {
    Future<size_t> read = io::read(pipes[0], temp, 128);
    AWAIT_READY(read);
    length += read.get();
  }

  AWAIT_EXPECT_READY(future1);

  ASSERT_SOME(os::close(pipes[0]));
  ASSERT_SOME(os::close(pipes[1]));
}


TEST_F(IOTest, Redirect)
{
  // Start by checking that using "invalid" file descriptors fails.
  AWAIT_EXPECT_FAILED(io::redirect(-1, 0));
  AWAIT_EXPECT_FAILED(io::redirect(0, -1));

  // Create a temporary file for redirecting into.
  string path = path::join(sandbox.get(), "output");

  Try<int_fd> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(fd);

  ASSERT_SOME(io::prepare_async(fd.get()));

  // Use a nonblocking pipe for doing the redirection.
  Try<array<int_fd, 2>> pipes_ = os::pipe();
  ASSERT_SOME(pipes_);

  array<int_fd, 2> pipes = pipes_.get();
  ASSERT_SOME(io::prepare_async(pipes[0]));
  ASSERT_SOME(io::prepare_async(pipes[1]));

  // Set up a redirect hook to also accumlate the data that we splice.
  string accumulated;
  lambda::function<void(const string&)> hook =
    [&accumulated](const string& data) {
      accumulated += data;
    };

  // Now write data to the pipe and splice to the file and the redirect hook.
  string data =
    "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
    "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
    "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in "
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
    "culpa qui officia deserunt mollit anim id est laborum.";

  // Create more data!
  while (Bytes(data.size()) < Megabytes(1)) {
    data.append(data);
  }

  Future<Nothing> redirect = io::redirect(pipes[0], fd.get(), 4096, {hook});

  // Closing the read end of the pipe and the file should not have any
  // impact as we dup the file descriptor.
  ASSERT_SOME(os::close(pipes[0]));
  ASSERT_SOME(os::close(fd.get()));

  EXPECT_TRUE(redirect.isPending());

  // Writing the data should keep the future pending as it hasn't seen
  // EOF yet.
  AWAIT_READY(io::write(pipes[1], data));

  EXPECT_TRUE(redirect.isPending());

  // Now closing the write pipe should cause an EOF on the read end,
  // thus completing underlying splice in io::redirect.
  ASSERT_SOME(os::close(pipes[1]));

  AWAIT_READY(redirect);

  // Now make sure all the data is in the file!
  Try<string> read = os::read(path);
  ASSERT_SOME(read);
  EXPECT_EQ(data, read.get());

  // Also make sure the data was properly
  // accumulated in the redirect hook.
  EXPECT_EQ(data, accumulated);
}
