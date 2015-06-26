#include <string>

#include <stout/hashset.hpp>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

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


TEST(HashsetTest, Union)
{
  hashset<int> hs1;
  hs1.insert(1);
  hs1.insert(2);
  hs1.insert(3);

  hashset<int> hs2;
  hs2.insert(3);
  hs2.insert(4);
  hs2.insert(5);

  hashset<int> hs3 = hs1 | hs2;

  ASSERT_EQ(5u, hs3.size());
  ASSERT_TRUE(hs3.contains(1));
  ASSERT_TRUE(hs3.contains(2));
  ASSERT_TRUE(hs3.contains(3));
  ASSERT_TRUE(hs3.contains(4));
  ASSERT_TRUE(hs3.contains(5));
}
