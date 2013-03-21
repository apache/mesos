#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <string>

#include <stout/uuid.hpp>

using std::string;


TEST(UUIDTest, test)
{
  UUID uuid1 = UUID::random();
  UUID uuid2 = UUID::fromBytes(uuid1.toBytes());
  UUID uuid3 = uuid2;

  EXPECT_EQ(uuid1, uuid2);
  EXPECT_EQ(uuid2, uuid3);
  EXPECT_EQ(uuid1, uuid3);

  string bytes1 = uuid1.toBytes();
  string bytes2 = uuid2.toBytes();
  string bytes3 = uuid3.toBytes();

  EXPECT_EQ(bytes1, bytes2);
  EXPECT_EQ(bytes2, bytes3);
  EXPECT_EQ(bytes1, bytes3);

  string string1 = uuid1.toString();
  string string2 = uuid2.toString();
  string string3 = uuid3.toString();

  EXPECT_EQ(string1, string2);
  EXPECT_EQ(string2, string3);
  EXPECT_EQ(string1, string3);
}
