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
  Try<set<string> > links = net::links();
  ASSERT_SOME(links);

  foreach(const string& link, links.get()) {
    Result<net::IP> ip = net::fromLinkDevice(link);
    EXPECT_FALSE(ip.isError());

    if (ip.isSome()) {
      string addr = stringify(ip.get());

      if (ip.get().prefix().isSome()) {
        string prefix = addr.substr(addr.find("/") + 1);
        ASSERT_SOME(numify<size_t>(prefix));
        EXPECT_EQ(ip.get().prefix().get(), numify<size_t>(prefix).get());
      }

      vector<string> tokens =
        strings::split(addr.substr(0, addr.find("/")), ".");

      EXPECT_EQ(4u, tokens.size());

      foreach (const string& token, tokens) {
        ASSERT_SOME(numify<int>(token));
        EXPECT_LE(0, numify<int>(token).get());
        EXPECT_GE(255, numify<int>(token).get());
      }
    }
  }

  Result<net::IP> ip = net::fromLinkDevice("non-exist");
  EXPECT_ERROR(ip);
}


TEST(NetTest, ConstructIP)
{
  EXPECT_SOME(net::IP::fromDotDecimal("127.0.0.1"));
  EXPECT_SOME(net::IP::fromDotDecimal("10.135.0.1/8"));
  EXPECT_SOME(net::IP::fromDotDecimal("192.168.1.1/16"));
  EXPECT_SOME(net::IP::fromDotDecimal("01.02.03.04"));
  EXPECT_SOME(net::IP::fromDotDecimal("172.39.13.123/31"));

  EXPECT_ERROR(net::IP::fromDotDecimal("123.1.1.2//23"));
  EXPECT_ERROR(net::IP::fromDotDecimal("121.2.3.5/23/"));
  EXPECT_ERROR(net::IP::fromDotDecimal("12.32.3.5/123"));
  EXPECT_ERROR(net::IP::fromDotDecimal("12.32.3.a/16"));
  EXPECT_ERROR(net::IP::fromDotDecimal("hello world"));
  EXPECT_ERROR(net::IP::fromDotDecimal("hello moto/8"));

  EXPECT_SOME(net::IP::fromAddressNetmask(0x12345678, 0xffff0000));
  EXPECT_SOME(net::IP::fromAddressNetmask(0x12345678, 0xf0000000));
  EXPECT_SOME(net::IP::fromAddressNetmask(0x12345678, 0xffffffff));
  EXPECT_SOME(net::IP::fromAddressNetmask(0x12345678, 0));

  EXPECT_ERROR(net::IP::fromAddressNetmask(0x087654321, 0xff));
  EXPECT_ERROR(net::IP::fromAddressNetmask(0x087654321, 0xff00ff00));

  EXPECT_SOME(net::IP::fromAddressPrefix(0x12345678, 16));
  EXPECT_SOME(net::IP::fromAddressPrefix(0x12345678, 32));
  EXPECT_SOME(net::IP::fromAddressPrefix(0x12345678, 0));

  EXPECT_ERROR(net::IP::fromAddressPrefix(0x12345678, 123));

  uint32_t address = 0x01020304;
  uint32_t netmask = 0xff000000;

  Try<net::IP> ip1 = net::IP::fromAddressNetmask(address, netmask);
  ASSERT_SOME(ip1);
  EXPECT_EQ(address, ip1.get().address());
  EXPECT_SOME_EQ(netmask, ip1.get().netmask());
  EXPECT_EQ("1.2.3.4/8", stringify(ip1.get()));

  Try<net::IP> ip2 = net::IP::fromDotDecimal(stringify(ip1.get()));
  ASSERT_SOME(ip2);
  EXPECT_EQ(ip1.get(), ip2.get());
}
