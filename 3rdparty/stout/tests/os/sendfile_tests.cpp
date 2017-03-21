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
  virtual void SetUp()
  {
    TemporaryDirectoryTest::SetUp();

    filename = "lorem.txt";

    ASSERT_SOME(os::write(filename, LOREM_IPSUM));
  }

  const string LOREM_IPSUM;
  string filename;
};


TEST_F(OsSendfileTest, Sendfile)
{
  Try<int_fd> fd = os::open(filename, O_RDONLY | O_CLOEXEC);
  ASSERT_SOME(fd);

  // Construct a socket pair and use sendfile to transmit the text.
  int s[2];
  ASSERT_NE(-1, socketpair(AF_UNIX, SOCK_STREAM, 0, s)) << os::strerror(errno);
  Try<ssize_t, SocketError> length =
    os::sendfile(s[0], fd.get(), 0, LOREM_IPSUM.size());
  ASSERT_TRUE(length.isSome());
  ASSERT_EQ(static_cast<ssize_t>(LOREM_IPSUM.size()), length.get());

  char* buffer = new char[LOREM_IPSUM.size()];
  ASSERT_EQ(static_cast<ssize_t>(LOREM_IPSUM.size()),
            read(s[1], buffer, LOREM_IPSUM.size()));
  ASSERT_EQ(LOREM_IPSUM, string(buffer, LOREM_IPSUM.size()));
  ASSERT_SOME(os::close(fd.get()));
  delete[] buffer;

  // Now test with a closed socket, the SIGPIPE should be suppressed!
  fd = os::open(filename, O_RDONLY | O_CLOEXEC);
  ASSERT_SOME(fd);
  ASSERT_SOME(os::close(s[1]));

  Try<ssize_t, SocketError> result =
    os::sendfile(s[0], fd.get(), 0, LOREM_IPSUM.size());
  int _errno = result.error().code;
  ASSERT_ERROR(result);

#ifdef __linux__
  ASSERT_EQ(EPIPE, _errno) << result.error().message;
#elif defined __APPLE__
  ASSERT_EQ(ENOTCONN, _errno) << result.error().message;
#endif

  ASSERT_SOME(os::close(fd.get()));
  ASSERT_SOME(os::close(s[0]));
}
