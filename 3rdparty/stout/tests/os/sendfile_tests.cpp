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

#include <string>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <stout/os/sendfile.hpp>
#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

using std::string;

class OsSendfileTest : public TemporaryDirectoryTest
{
public:
  OsSendfileTest()
    : LOREM_IPSUM(
        "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
        "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
        "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
        "aliquip ex ea commodo consequat. Duis aute irure dolor in "
        "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
        "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
        "culpa qui officia deserunt mollit anim id est laborum.") {}

protected:
  void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    filename = "lorem.txt";

    ASSERT_SOME(os::write(filename, LOREM_IPSUM));
  }

  const string LOREM_IPSUM;
  string filename;
};


#ifdef __WINDOWS__
// TODO(akagup): If it's necessary, we can have a more general version of this
// function in a stout header, but `socketpair` isn't currently used in the
// cross-platform parts of Mesos.
Try<std::array<int_fd, 2>> socketpair()
{
  struct AutoFD {
    int_fd fd;
    ~AutoFD() {
      os::close(fd);
    }
  };

  const Try<int_fd> server_ = net::socket(AF_INET, SOCK_STREAM, 0);
  if (server_.isError()) {
    return Error(server_.error());
  }
  const AutoFD server{server_.get()};

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (net::bind(
          server.fd,
          reinterpret_cast<const sockaddr*>(&addr),
          sizeof(addr)) != 0) {
    return SocketError();
  }

  if (::listen(server.fd, 1) != 0) {
    return SocketError();
  }

  int addrlen = sizeof(addr);
  if (::getsockname(
          server.fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) != 0) {
    return SocketError();
  }

  const Try<int_fd> client_ = net::socket(AF_INET, SOCK_STREAM, 0);
  if (client_.isError()) {
    return Error(client_.error());
  }

  // Don't use the `AutoFD` here, since we want to return this and not call
  // the destructor.
  const int_fd client = client_.get();

  // `connect` won't block due to the listening server backlog.
  if (net::connect(
          client,
          reinterpret_cast<const sockaddr*>(&addr),
          sizeof(addr)) != 0) {
    SocketError error;
    os::close(client);
    return error;
  }

  // Don't use the `AutoFD` here, since we want to return this and not call
  // the destructor.
  addrlen = sizeof(addr);
  const int_fd accepted_client =
    net::accept(server.fd, reinterpret_cast<sockaddr*>(&addr), &addrlen);

  if (!accepted_client.is_valid()) {
    SocketError error;
    os::close(client);
    return error;
  }

  return std::array<int_fd, 2>{ accepted_client, client };
}
#endif // __WINDOWS__


TEST_F(OsSendfileTest, Sendfile)
{
  Try<int_fd> fd = os::open(filename, O_RDONLY | O_CLOEXEC);
  ASSERT_SOME(fd);

  // Construct a socket pair and use sendfile to transmit the text.
  int_fd s[2];

#ifdef __WINDOWS__
  Try<std::array<int_fd, 2>> s_ = socketpair();
#else
  Try<std::array<int_fd, 2>> s_ = net::socketpair(AF_UNIX, SOCK_STREAM, 0);
#endif // __WINDOWS__

  ASSERT_SOME(s_);
  s[0] = s_.get()[0];
  s[1] = s_.get()[1];

  Try<ssize_t, SocketError> length =
    os::sendfile(s[0], fd.get(), 0, LOREM_IPSUM.size());
  ASSERT_SOME_EQ(static_cast<ssize_t>(LOREM_IPSUM.size()), length);

  char* buffer = new char[LOREM_IPSUM.size()];
  ASSERT_EQ(static_cast<ssize_t>(LOREM_IPSUM.size()),
            os::read(s[1], buffer, LOREM_IPSUM.size()));
  ASSERT_EQ(LOREM_IPSUM, string(buffer, LOREM_IPSUM.size()));
  ASSERT_SOME(os::close(fd.get()));
  delete[] buffer;

  // Now test with a closed socket, the SIGPIPE should be suppressed!
  fd = os::open(filename, O_RDONLY | O_CLOEXEC);
  ASSERT_SOME(fd);
  ASSERT_SOME(os::close(s[1]));

  Try<ssize_t, SocketError> result =
    os::sendfile(s[0], fd.get(), 0, LOREM_IPSUM.size());
  ASSERT_ERROR(result);
  int _errno = result.error().code;

#ifdef __linux__
  ASSERT_EQ(EPIPE, _errno) << result.error().message;
#elif defined __APPLE__
  ASSERT_EQ(ENOTCONN, _errno) << result.error().message;
#elif defined __WINDOWS__
  ASSERT_EQ(WSAECONNRESET, _errno) << result.error().message;
#endif

  ASSERT_SOME(os::close(fd.get()));

#ifdef __WINDOWS__
  // On Windows, closing this socket results in the `WSACONNRESET` error.
  ASSERT_ERROR(os::close(s[0]));
#else
  ASSERT_SOME(os::close(s[0]));
#endif // __WINDOWS__
}

#ifdef __WINDOWS__
TEST_F(OsSendfileTest, SendfileAsync)
{
  const string testfile =
    path::join(sandbox.get(), id::UUID::random().toString());

  // We create a 1MB file, which should be too big for the socket
  // buffers to hold, forcing us to go through the asynchronous path.
  const Try<int_fd> fd = os::open(testfile, O_CREAT | O_TRUNC | O_RDWR);
  string data(1024 * 1024, 'A');
  ASSERT_SOME(fd);
  ASSERT_SOME(os::write(fd.get(), data));
  ASSERT_SOME(os::lseek(fd.get(), 0, SEEK_SET));

  const Try<std::array<int_fd, 2>> s = socketpair();

  // We are sending data, but the other side isn't reading, so we should get
  // that the operation is pending.
  OVERLAPPED overlapped = {};
  const Result<size_t> length =
    os::sendfile_async(s.get()[0], fd.get(), data.size(), &overlapped);

  // NOTE: A pending operation is represented by None()
  ASSERT_NONE(length);

  const Result<string> result = os::read(s.get()[1], data.size());
  ASSERT_SOME(result);
  ASSERT_EQ(data, result.get());

  DWORD sent = 0;
  DWORD flags = 0;
  ASSERT_TRUE(
      ::WSAGetOverlappedResult(s.get()[0], &overlapped, &sent, TRUE, &flags));

  EXPECT_SOME(os::close(fd.get()));
  EXPECT_SOME(os::close(s.get()[0]));
  EXPECT_SOME(os::close(s.get()[1]));
}
#endif // __WINDOWS__
