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
#include <stout/ip.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

using std::set;
using std::string;
using std::vector;


TEST(NetTest, LinkDevice)
{
  Try<set<string>> links = net::links();
  ASSERT_SOME(links);

  foreach (const string& link, links.get()) {
    Result<net::IP::Network> network =
      net::IP::Network::fromLinkDevice(link, AF_INET);

    EXPECT_FALSE(network.isError());

    if (network.isSome()) {
      string addr = stringify(network.get());
      string prefix = addr.substr(addr.find('/') + 1);
      ASSERT_SOME(numify<int>(prefix));
      EXPECT_EQ(network->prefix(), numify<int>(prefix).get());

      vector<string> tokens =
        strings::split(addr.substr(0, addr.find('/')), ".");

      EXPECT_EQ(4u, tokens.size());

      foreach (const string& token, tokens) {
        ASSERT_SOME(numify<int>(token));
        EXPECT_LE(0, numify<int>(token).get());
        EXPECT_GE(255, numify<int>(token).get());
      }
    }
  }

  EXPECT_ERROR(net::IP::Network::fromLinkDevice("non-exist", AF_INET));
}


TEST(NetTest, ConstructIPv4)
{
  EXPECT_SOME(net::IP::parse("127.0.0.1", AF_INET));
  EXPECT_SOME(net::IP::parse("1.2.3.4", AF_INET));

  EXPECT_ERROR(net::IP::parse("123.1.1..2", AF_INET));
  EXPECT_ERROR(net::IP::parse("121.2.3.5.", AF_INET));
  EXPECT_ERROR(net::IP::parse("12.32.3.a", AF_INET));
  EXPECT_ERROR(net::IP::parse("hello world", AF_INET));

  uint32_t address = 0x01020304;
  Try<net::IPv4> ip4 = net::IPv4::parse("1.2.3.4");
  ASSERT_SOME(ip4);
  EXPECT_EQ(ip4.get(), net::IPv4(address));

  Try<net::IPv4> loopback = net::IPv4::parse("127.0.0.1");
  ASSERT_SOME(loopback);
  EXPECT_TRUE(loopback->isLoopback());

  Try<net::IPv4> any = net::IPv4::parse("0.0.0.0");
  ASSERT_SOME(any);
  EXPECT_TRUE(any->isAny());

  EXPECT_NE(loopback.get(), any.get());
  EXPECT_GT(loopback.get(), any.get());
  EXPECT_LT(any.get(), loopback.get());
}


TEST(NetTest, ConstructIPv6)
{
  EXPECT_SOME(net::IP::parse("::1", AF_INET6));
  EXPECT_SOME(net::IP::parse("fe80::eef4:bbff:fe33:a9c7", AF_INET6));
  EXPECT_SOME(net::IPv6::parse("fe80::eef4:bbff:fe33:a9c7"));
  EXPECT_SOME(net::IP::parse("::192.9.5.5", AF_INET6));
  EXPECT_SOME(net::IPv6::parse("::192.9.5.5"));

  EXPECT_ERROR(net::IP::parse("1::1::1", AF_INET6));
  EXPECT_ERROR(net::IPv6::parse("1::1::1"));
  EXPECT_ERROR(net::IP::parse("121.2.3.5", AF_INET6));
  EXPECT_ERROR(net::IPv6::parse("121.2.3.5"));
  EXPECT_ERROR(net::IP::parse("fe80:2:a", AF_INET6));
  EXPECT_ERROR(net::IPv6::parse("fe80:2:a"));
  EXPECT_ERROR(net::IP::parse("hello world", AF_INET6));
  EXPECT_ERROR(net::IPv6::parse("hello world"));

  Try<net::IPv6> loopback = net::IPv6::parse("::1");
  ASSERT_SOME(loopback);
  net::IPv6 loopback2 = net::IPv6(loopback->in6());
  EXPECT_EQ(loopback2, loopback.get());
  EXPECT_TRUE(loopback2.isLoopback());
  EXPECT_FALSE(loopback2.isAny());

  Try<net::IPv6> any = net::IPv6::parse("::");
  ASSERT_SOME(any);
  EXPECT_TRUE(any->isAny());
  EXPECT_FALSE(any->isLoopback());

  EXPECT_NE(loopback.get(), any.get());
  EXPECT_GT(loopback.get(), any.get());
  EXPECT_LT(any.get(), loopback.get());
}


TEST(NetTest, ConstructIPv4Network)
{
  EXPECT_SOME(net::IP::Network::parse("10.135.0.1/8"));
  EXPECT_SOME(net::IP::Network::parse("192.168.1.1/16"));
  EXPECT_SOME(net::IP::Network::parse("172.39.13.123/31"));

  EXPECT_ERROR(net::IP::Network::parse("123.1.1.2//23"));
  EXPECT_ERROR(net::IP::Network::parse("121.2.3.5/23/"));
  EXPECT_ERROR(net::IP::Network::parse("12.32.3.a/16"));
  EXPECT_ERROR(net::IP::Network::parse("hello moto/8"));
  EXPECT_ERROR(net::IP::Network::parse("::1/128", AF_INET));

  EXPECT_SOME(net::IP::Network::create(
      net::IP(0x12345678),
      net::IP(0xffff0000)));

  EXPECT_SOME(net::IP::Network::create(
      net::IP(0x12345678),
      net::IP(0xf0000000)));

  EXPECT_SOME(net::IP::Network::create(
      net::IP(0x12345678),
      net::IP(0xffffffff)));

  EXPECT_SOME(net::IP::Network::create(net::IP(0x12345678), net::IP(0)));

  EXPECT_ERROR(net::IP::Network::create(net::IP(0x87654321), net::IP(0xff)));
  EXPECT_ERROR(net::IP::Network::create(
      net::IP(0x87654321),
      net::IP(0xff00ff)));

  Try<net::IP::Network> n1 = net::IP::Network::create(net::IP(0x12345678), 16);
  Try<net::IP::Network> n2 = net::IP::Network::create(net::IP(0x12345678), 32);
  Try<net::IP::Network> n3 = net::IP::Network::create(net::IP(0x12345678), 0);

  EXPECT_SOME_EQ(net::IP::Network::parse("18.52.86.120/16", AF_INET).get(), n1);
  EXPECT_SOME_EQ(net::IP::Network::parse("18.52.86.120/32", AF_INET).get(), n2);
  EXPECT_SOME_EQ(net::IP::Network::parse("18.52.86.120/0", AF_INET).get(), n3);

  EXPECT_ERROR(net::IP::Network::create(net::IP(0x12345678), 123));
  EXPECT_ERROR(net::IP::Network::create(net::IP(0x12345678), -1));

  uint32_t address = 0x01020304;
  uint32_t netmask = 0xff000000;

  Try<net::IP::Network> network =
      net::IP::Network::create(net::IP(address), net::IP(netmask));

  ASSERT_SOME(network);
  EXPECT_EQ(net::IP(address), network->address());
  EXPECT_EQ(net::IP(netmask), network->netmask());
  EXPECT_EQ("1.2.3.4/8", stringify(network.get()));

  Try<net::IP::Network> network2 =
      net::IP::Network::parse(stringify(network.get()));

  ASSERT_SOME(network2);
  EXPECT_EQ(network.get(), network2.get());
}


TEST(NetTest, ConstructIPv6Network)
{
  EXPECT_SOME(net::IP::Network::parse("::/128"));
  EXPECT_SOME(net::IP::Network::parse("fe80::d/64"));
  EXPECT_SOME(net::IP::Network::parse(
      "2001:cdba:0000:0000:0000:0000:3257:9652/16"));

  EXPECT_ERROR(net::IP::Network::parse("10.135.0.1/8", AF_INET6));
  EXPECT_ERROR(net::IP::Network::parse("hello moto/8"));

  net::IP loopback(::in6addr_loopback);
  ASSERT_EQ("::1", stringify(loopback));

  Try<net::IP> address = net::IP::parse("2001:cdba::3257:9652");
  Try<net::IP> netmask1 = net::IP::parse("ff80::"); // 9 bits
  Try<net::IP> netmask2 = net::IP::parse("ffff:ffff:e000::"); // 35 bits
  ASSERT_SOME(address);
  ASSERT_SOME(netmask1);
  ASSERT_SOME(netmask2);

  EXPECT_SOME(net::IP::Network::create(address.get(), netmask1.get()));
  EXPECT_ERROR(net::IP::Network::create(address.get(), loopback));

  Try<net::IP::Network> n1 = net::IP::Network::create(loopback, 53);
  Try<net::IP::Network> n2 = net::IP::Network::create(loopback, 128);
  Try<net::IP::Network> n3 = net::IP::Network::create(loopback, 0);

  EXPECT_SOME_EQ(net::IP::Network::parse("::1/53", AF_INET6).get(), n1);
  EXPECT_SOME_EQ(net::IP::Network::parse("::1/128", AF_INET6).get(), n2);
  EXPECT_SOME_EQ(net::IP::Network::parse("::1/0", AF_INET6).get(), n3);

  EXPECT_ERROR(net::IP::Network::create(loopback, -3));
  EXPECT_ERROR(net::IP::Network::create(loopback, 182));

  Try<net::IP::Network> network =
    net::IP::Network::create(address.get(), netmask1.get());

  ASSERT_SOME(network);
  EXPECT_EQ(address.get(), network->address());
  EXPECT_EQ(netmask1.get(), network->netmask());
  EXPECT_EQ("2001:cdba::3257:9652/9", stringify(network.get()));

  Try<net::IP::Network> network2 =
    net::IP::Network::parse(stringify(network.get()));

  ASSERT_SOME(network2);
  EXPECT_EQ(network.get(), network2.get());
}


// Check the copy assignment operator for `net::IP::Network` works.
TEST(NetTest, CopyIPNetwork)
{
  Try<net::IP::Network> network = net::IP::Network::parse("192.168.1.1/16");
  EXPECT_SOME(network);

  net::IP::Network network2 = network.get();
  EXPECT_EQ(network2, network.get());

  // Check the same for IPv6.
  network = net::IP::Network::parse("::1/128");
  EXPECT_SOME(network);

  network2 = network.get();
  EXPECT_EQ(network2, network.get());
}
