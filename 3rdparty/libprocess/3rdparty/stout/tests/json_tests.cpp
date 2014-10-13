#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stdint.h>

#include <sys/stat.h>

#include <string>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

using std::string;

using boost::get;


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


TEST(JsonTest, BooleanFormat)
{
  EXPECT_EQ("false", stringify(JSON::False()));
  EXPECT_EQ("true", stringify(JSON::True()));

  EXPECT_EQ("true", stringify(JSON::Boolean(true)));
  EXPECT_EQ("false", stringify(JSON::Boolean(false)));

  EXPECT_EQ("true", stringify(JSON::Value(true)));
  EXPECT_EQ("false", stringify(JSON::Value(false)));
}


TEST(JsonTest, BooleanAssignement)
{
  JSON::Value v = true;
  EXPECT_TRUE(get<JSON::Boolean>(v).value);

  v = false;
  EXPECT_FALSE(get<JSON::Boolean>(v).value);

  JSON::Boolean b = true;
  EXPECT_TRUE(b.value);

  b = false;
  EXPECT_FALSE(b.value);
}


TEST(JsonTest, CStringAssignment)
{
  JSON::Value v = "test";
  JSON::String s = "test";
  EXPECT_EQ(get<JSON::String>(v).value, "test");
  EXPECT_EQ(s.value, "test");

  v = "123";
  s = "123";
  EXPECT_EQ(get<JSON::String>(v).value, "123");
  EXPECT_EQ(s.value, "123");

  char buf[1000];

  v = strcpy(buf, "bar");
  s = strcpy(buf, "bar");
  EXPECT_EQ(get<JSON::String>(v).value, "bar");
  EXPECT_EQ(s.value, "bar");
}


TEST(JsonTest, NumericAssignment)
{
  // Just using this to get various numeric datatypes that
  // are used by clients of stout.
  struct stat s;

  s.st_nlink = 1;
  JSON::Value v = s.st_nlink;
  JSON::Number d = s.st_nlink;
  EXPECT_EQ(get<JSON::Number>(v).value, 1.0);
  EXPECT_EQ(d.value, 1.0);

  s.st_size = 2;
  v = s.st_size;
  d = s.st_size;
  EXPECT_EQ(get<JSON::Number>(v).value, 2.0);
  EXPECT_EQ(d.value, 2.0);

  s.st_mtime = 3;
  v = s.st_mtime;
  d = s.st_mtime;
  EXPECT_EQ(get<JSON::Number>(v).value, 3.0);
  EXPECT_EQ(d.value, 3.0);

  size_t st = 4;
  v = st;
  d = st;
  EXPECT_EQ(get<JSON::Number>(v).value, 4.0);
  EXPECT_EQ(d.value, 4.0);

  uint64_t ui64 = 5;
  v = ui64;
  d = ui64;
  EXPECT_EQ(get<JSON::Number>(v).value, 5.0);
  EXPECT_EQ(d.value, 5.0);

  const unsigned int ui = 6;
  v = ui;
  d = ui;
  EXPECT_EQ(get<JSON::Number>(v).value, 6.0);
  EXPECT_EQ(d.value, 6.0);

  int i = 7;
  v = i;
  d = i;
  EXPECT_EQ(get<JSON::Number>(v).value, 7.0);
  EXPECT_EQ(d.value, 7.0);
}


TEST(JsonTest, parse)
{
  JSON::Object object;

  object.values["strings"] = "string";
  object.values["integer1"] = 1;
  object.values["integer2"] = -1;
  object.values["double1"] = 1;
  object.values["double2"] = -1;
  object.values["double3"] = -1.42;

  JSON::Object nested;
  nested.values["string"] = "string";

  EXPECT_SOME_EQ(nested, JSON::parse<JSON::Object>(stringify(nested)));

  object.values["nested"] = nested;

  JSON::Array array;
  array.values.push_back(nested);

  EXPECT_SOME_EQ(array, JSON::parse<JSON::Array>(stringify(array)));

  object.values["array"] = array;

  EXPECT_SOME_EQ(object, JSON::parse(stringify(object)));
}


TEST(JsonTest, Find)
{
  JSON::Object object;

  JSON::Object nested1;

  JSON::Object nested2;
  nested2.values["string"] = "string";
  nested2.values["integer"] = -1;
  nested2.values["double"] = -1.42;
  nested2.values["null"] = JSON::Null();

  JSON::Array array;
  array.values.push_back("hello");

  nested2.values["array"] = array;

  nested1.values["nested2"] = nested2;

  object.values["nested1"] = nested1;

  ASSERT_NONE(object.find<JSON::String>("nested.nested.string"));
  ASSERT_NONE(object.find<JSON::String>("nested1.nested2.none"));

  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array.foo"));

  ASSERT_SOME_EQ(
      JSON::String("string"),
      object.find<JSON::String>("nested1.nested2.string"));

  ASSERT_SOME_EQ(
      JSON::Number(-1),
      object.find<JSON::Number>("nested1.nested2.integer"));

  ASSERT_SOME_EQ(
      JSON::Number(-1.42),
      object.find<JSON::Number>("nested1.nested2.double"));

  ASSERT_SOME_EQ(
      JSON::Null(),
      object.find<JSON::Null>("nested1.nested2.null"));

  ASSERT_SOME_EQ(
      JSON::String("hello"),
      object.find<JSON::String>("nested1.nested2.array[0]"));

  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[1"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[[1]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[1]]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array.[1]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[.1]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[1.]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[[]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[]]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[[]]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[[1]]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[[1]"));
  ASSERT_ERROR(object.find<JSON::String>("nested1.nested2.array[1]]"));

  // Out of bounds is none.
  ASSERT_NONE(object.find<JSON::String>("nested1.nested2.array[1]"));

  // Also test getting JSON::Value when you don't know the type.
  ASSERT_SOME(object.find<JSON::Value>("nested1.nested2.null"));
}
