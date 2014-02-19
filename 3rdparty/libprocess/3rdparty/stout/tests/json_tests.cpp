#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <string>

#include <stout/json.hpp>
#include <stout/stringify.hpp>

using std::string;


TEST(JsonTest, DefaultValueIsNull)
{
  JSON::Value v;
  EXPECT_EQ("null", stringify(v));
}


TEST(JsonTest, BinaryData)
{
  JSON::String s(string("\"\\/\b\f\n\r\t\x00\x19 !#[]\x7F\xFF", 17));

  EXPECT_EQ("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\\u0019 !#[]\\u007F\\u00FF\"",
            stringify(s));
}


TEST(JsonTest, NumberFormat)
{
  // Test whole numbers.
  EXPECT_EQ("0", stringify(JSON::Number(0.0)));
  EXPECT_EQ("1", stringify(JSON::Number(1.0)));

  // Negative.
  EXPECT_EQ("-1", stringify(JSON::Number(-1.0)));

  // Expect at least 15 digits of precision.
  EXPECT_EQ("1234567890.12345", stringify(JSON::Number(1234567890.12345)));
}
