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
#include <stout/os/socket.hpp>

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
