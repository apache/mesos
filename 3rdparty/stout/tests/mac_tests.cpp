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

#include <stdio.h>

#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/mac.hpp>
#include <stout/net.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

using std::set;
using std::string;
using std::vector;


TEST(NetTest, Mac)
{
  Try<set<string>> links = net::links();
  ASSERT_SOME(links);

  foreach (const string& link, links.get()) {
    Result<net::MAC> mac = net::mac(link);
    EXPECT_FALSE(mac.isError());

    if (mac.isSome()) {
      EXPECT_NE("00:00:00:00:00:00", stringify(mac.get()));

      vector<string> tokens = strings::split(stringify(mac.get()), ":");
      EXPECT_EQ(6u, tokens.size());

      for (size_t i = 0; i < tokens.size(); i++) {
        EXPECT_EQ(2u, tokens[i].size());

        uint8_t value;
        ASSERT_EQ(1, sscanf(tokens[i].c_str(), "%hhx", &value));
        EXPECT_EQ(value, mac.get()[i]);
      }
    }
  }

  Result<net::MAC> mac = net::mac("non-exist");
  EXPECT_ERROR(mac);
}


TEST(NetTest, ConstructMAC)
{
  uint8_t bytes[6] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};

  EXPECT_EQ("12:34:56:78:9a:bc", stringify(net::MAC(bytes)));
}


TEST(NetTest, ParseMAC)
{
  Try<net::MAC> mac = net::MAC::parse("12:34:56:78:9a:bc");
  ASSERT_SOME(mac);

  EXPECT_EQ(0x12, mac.get()[0]);
  EXPECT_EQ(0x34, mac.get()[1]);
  EXPECT_EQ(0x56, mac.get()[2]);
  EXPECT_EQ(0x78, mac.get()[3]);
  EXPECT_EQ(0x9a, mac.get()[4]);
  EXPECT_EQ(0xbc, mac.get()[5]);

  EXPECT_SOME(net::MAC::parse("12:34:56:78:9A:BC"));
  EXPECT_SOME(net::MAC::parse("ff:34:56:78:9a:bc"));
  EXPECT_SOME(net::MAC::parse("00:34:56:78:9a:bc"));
  EXPECT_ERROR(net::MAC::parse("12:34:56:78:9a"));
  EXPECT_ERROR(net::MAC::parse("123:34:56:78:9a:bc"));
  EXPECT_ERROR(net::MAC::parse("x1:34:56:78:9a:bc"));
  EXPECT_ERROR(net::MAC::parse("-1:34:56:78:9a:bc"));
  EXPECT_ERROR(net::MAC::parse("+1:34:56:78:9a:bc"));
  EXPECT_ERROR(net::MAC::parse("fff:34:56:78:9a:bc"));
  EXPECT_ERROR(net::MAC::parse(" 2:34:56:78:9a:bc"));
  EXPECT_ERROR(net::MAC::parse("1 :34:56:78:9a:bc"));
}
