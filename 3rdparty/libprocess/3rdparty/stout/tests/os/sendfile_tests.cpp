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


TEST_F(OsSendfileTest, sendfile)
{
  Try<int> fd = os::open(filename, O_RDONLY | O_CLOEXEC);
  ASSERT_SOME(fd);

  // Construct a socket pair and use sendfile to transmit the text.
  int s[2];
  ASSERT_NE(-1, socketpair(AF_UNIX, SOCK_STREAM, 0, s)) << strerror(errno);
  ASSERT_EQ(
      LOREM_IPSUM.size(),
      os::sendfile(s[0], fd.get(), 0, LOREM_IPSUM.size()));

  char* buffer = new char[LOREM_IPSUM.size()];
  ASSERT_EQ(LOREM_IPSUM.size(), read(s[1], buffer, LOREM_IPSUM.size()));
  ASSERT_EQ(LOREM_IPSUM, string(buffer, LOREM_IPSUM.size()));
  ASSERT_SOME(os::close(fd.get()));
  delete[] buffer;

  // Now test with a closed socket, the SIGPIPE should be suppressed!
  fd = os::open(filename, O_RDONLY | O_CLOEXEC);
  ASSERT_SOME(fd);
  ASSERT_SOME(os::close(s[1]));

  ssize_t result = os::sendfile(s[0], fd.get(), 0, LOREM_IPSUM.size());
  int _errno = errno;
  ASSERT_EQ(-1, result);

#ifdef __linux__
  ASSERT_EQ(EPIPE, _errno) << strerror(_errno);
#elif defined __APPLE__
  ASSERT_EQ(ENOTCONN, _errno) << strerror(_errno);
#endif

  ASSERT_SOME(os::close(fd.get()));
  ASSERT_SOME(os::close(s[0]));
}
