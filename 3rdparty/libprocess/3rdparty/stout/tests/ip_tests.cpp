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
    Result<net::IPNetwork> network = net::fromLinkDevice(link);
    EXPECT_FALSE(network.isError());

    if (network.isSome()) {
      string addr = stringify(network.get());
      string prefix = addr.substr(addr.find("/") + 1);
      ASSERT_SOME(numify<size_t>(prefix));
      EXPECT_EQ(network.get().prefix().get(), numify<size_t>(prefix).get());

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

  EXPECT_ERROR(net::fromLinkDevice("non-exist"));
}


TEST(NetTest, ConstructIP)
{
  EXPECT_SOME(net::IP::parse("127.0.0.1"));
  EXPECT_SOME(net::IP::parse("01.02.03.04"));

  EXPECT_ERROR(net::IP::parse("123.1.1..2"));
  EXPECT_ERROR(net::IP::parse("121.2.3.5."));
  EXPECT_ERROR(net::IP::parse("12.32.3.a"));
  EXPECT_ERROR(net::IP::parse("hello world"));
}


TEST(NetTest, ConstructIPNetwork)
{
  EXPECT_SOME(net::IPNetwork::parse("10.135.0.1/8"));
  EXPECT_SOME(net::IPNetwork::parse("192.168.1.1/16"));
  EXPECT_SOME(net::IPNetwork::parse("172.39.13.123/31"));

  EXPECT_ERROR(net::IPNetwork::parse("123.1.1.2//23"));
  EXPECT_ERROR(net::IPNetwork::parse("121.2.3.5/23/"));
  EXPECT_ERROR(net::IPNetwork::parse("12.32.3.a/16"));
  EXPECT_ERROR(net::IPNetwork::parse("hello moto/8"));

  EXPECT_SOME(net::IPNetwork::fromAddressNetmask(
      net::IP(0x12345678),
      net::IP(0xffff0000)));
  EXPECT_SOME(net::IPNetwork::fromAddressNetmask(
      net::IP(0x12345678),
      net::IP(0xf0000000)));
  EXPECT_SOME(net::IPNetwork::fromAddressNetmask(
      net::IP(0x12345678),
      net::IP(0xffffffff)));
  EXPECT_SOME(net::IPNetwork::fromAddressNetmask(
      net::IP(0x12345678),
      net::IP(0)));

  EXPECT_ERROR(net::IPNetwork::fromAddressNetmask(
      net::IP(0x087654321),
      net::IP(0xff)));
  EXPECT_ERROR(net::IPNetwork::fromAddressNetmask(
      net::IP(0x087654321),
      net::IP(0xff00ff00)));

  EXPECT_SOME(net::IPNetwork::fromAddressPrefix(net::IP(0x12345678), 16));
  EXPECT_SOME(net::IPNetwork::fromAddressPrefix(net::IP(0x12345678), 32));
  EXPECT_SOME(net::IPNetwork::fromAddressPrefix(net::IP(0x12345678), 0));

  EXPECT_ERROR(net::IPNetwork::fromAddressPrefix(net::IP(0x12345678), 123));

  uint32_t address = 0x01020304;
  uint32_t netmask = 0xff000000;

  Try<net::IPNetwork> ip1 = net::IPNetwork::fromAddressNetmask(
      net::IP(address),
      net::IP(netmask));
  ASSERT_SOME(ip1);
  EXPECT_EQ(net::IP(address), ip1.get().address());
  EXPECT_EQ(net::IP(netmask), ip1.get().netmask());
  EXPECT_EQ("1.2.3.4/8", stringify(ip1.get()));

  Try<net::IPNetwork> ip2 = net::IPNetwork::parse(stringify(ip1.get()));
  ASSERT_SOME(ip2);
  EXPECT_EQ(ip1.get(), ip2.get());
}
