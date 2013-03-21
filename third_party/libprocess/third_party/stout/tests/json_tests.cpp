#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <string>

#include <stout/json.hpp>
#include <stout/stringify.hpp>

using std::string;


TEST(JsonTest, BinaryData)
{
  JSON::String s(string("\"\\/\b\f\n\r\t\x00\x19 !#[]\x7F\xFF", 17));

  EXPECT_EQ("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\\u0019 !#[]\\u007F\\u00FF\"",
            stringify(s));
}
