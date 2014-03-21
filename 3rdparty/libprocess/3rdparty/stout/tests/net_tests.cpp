#include <stdio.h>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/net.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

using std::set;
using std::string;
using std::vector;


TEST(NetTest, mac)
{
  Try<set<string> > links = net::links();
  ASSERT_SOME(links);

  foreach(const string& link, links.get()) {
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
