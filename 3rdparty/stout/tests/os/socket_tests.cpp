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

#include <stout/os/int_fd.hpp>
#include <stout/os/mktemp.hpp>
#include <stout/os/mkdtemp.hpp>
#include <stout/os/rm.hpp>
#include <stout/os/rmdir.hpp>
#include <stout/os/socket.hpp>
#include <stout/os/stat.hpp>

#include <stout/tests/utils.hpp>

using std::string;


class SocketTests : public TemporaryDirectoryTest {};


#ifdef __WINDOWS__
TEST_F(SocketTests, InitSocket)
{
  // `wsa_initialize` should always return `true`.
  ASSERT_TRUE(net::wsa_initialize());
  ASSERT_TRUE(net::wsa_initialize());

  // `wsa_cleanup` should always return `true`.
  ASSERT_TRUE(net::wsa_cleanup());
  ASSERT_TRUE(net::wsa_cleanup());
}


TEST_F(SocketTests, IntFD)
{
  const int_fd fd(INVALID_SOCKET);
  EXPECT_EQ(int_fd::Type::SOCKET, fd.type());
  EXPECT_FALSE(fd.is_valid());
  EXPECT_EQ(fd, int_fd(-1));
  EXPECT_EQ(-1, fd);
  EXPECT_LT(fd, 0);
  EXPECT_GT(0, fd);
}
#endif // __WINDOWS__

#ifndef __WINDOWS__

#include <sys/un.h>

TEST_F(SocketTests, IsSocket)
{
  Try<std::string> nonsocketPath = os::mktemp();
  Try<std::string> nonsocketDir = os::mkdtemp();

  ASSERT_SOME(nonsocketPath);
  ASSERT_SOME(nonsocketDir);

  // Avoiding `ASSERT()` after this point to ensure we will arrive
  // at the `rmdir()` cleanup.

  int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_GE(socket, 0);

  const char* socketFilename = "/agent.sock";

  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  ::strncpy(&addr.sun_path[0], &nonsocketDir.get()[0], nonsocketDir->size()+1);
  ::strncat(&addr.sun_path[0], socketFilename, ::strlen(socketFilename)+1);

  EXPECT_NE(
    ::bind(socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
    -1);

  EXPECT_TRUE(os::stat::issocket(addr.sun_path));
  EXPECT_FALSE(os::stat::issocket(nonsocketPath.get()));
  EXPECT_FALSE(os::stat::issocket(nonsocketDir.get()));

  os::rm(nonsocketPath.get());
  os::rmdir(nonsocketDir.get());
}
#endif // __WINDOWS__
