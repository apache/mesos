#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <string>

#include <stout/hashset.hpp>

using std::string;


TEST(HashsetTest, Insert)
{
  hashset<string> hs1;
  hs1.insert(string("HS1"));
  hs1.insert(string("HS3"));

  hashset<string> hs2;
  hs2.insert(string("HS2"));

  hs1 = hs2;
  ASSERT_EQ(1u, hs1.size());
  ASSERT_TRUE(hs1.contains("HS2"));
  ASSERT_TRUE(hs1 == hs2);
}
