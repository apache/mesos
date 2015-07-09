/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include <stdint.h>

#include <sys/stat.h>

#include <string>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

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


TEST(JsonTest, Parse)
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


// Test the equality operator between two objects.
TEST(JsonTest, Equals)
{
  // Array checks.
  Try<JSON::Value> _array = JSON::parse("{\"array\" : [1, 2, 3]}");
  ASSERT_SOME(_array);
  const JSON::Value array = _array.get();

  EXPECT_SOME_EQ(array, JSON::parse("{\"array\" : [1, 2, 3]}"));
  EXPECT_SOME_NE(array, JSON::parse("{\"array\" : [3, 2, 1, 0]}"));
  EXPECT_SOME_NE(array, JSON::parse("{\"array\" : [1, 2, 3, 4]}"));
  EXPECT_SOME_NE(array, JSON::parse("{\"array\" : [3, 2, 1]}"));
  EXPECT_SOME_NE(array, JSON::parse("{\"array\" : [1, 2, 4]}"));
  EXPECT_SOME_NE(array, JSON::parse("{\"array\" : [1, 2]}"));
  EXPECT_SOME_NE(array, JSON::parse("{\"array\" : []}"));
  EXPECT_SOME_NE(array, JSON::parse("{\"array\" : null}"));
  EXPECT_SOME_NE(array, JSON::parse("{\"array\" : 42}"));

  // Boolean checks.
  Try<JSON::Value> _boolean = JSON::parse("{\"boolean\" : true}");
  ASSERT_SOME(_boolean);
  const JSON::Value boolean = _boolean.get();

  EXPECT_SOME_EQ(boolean, JSON::parse("{\"boolean\" : true}"));
  EXPECT_SOME_NE(boolean, JSON::parse("{\"boolean\" : false}"));
  EXPECT_SOME_NE(boolean, JSON::parse("{\"boolean\" : null}"));
  EXPECT_SOME_NE(boolean, JSON::parse("{\"boolean\" : 42}"));

  // Null checks.
  Try<JSON::Value> _nullEntry = JSON::parse("{\"null_entry\" : null}");
  ASSERT_SOME(_nullEntry);
  const JSON::Value nullEntry = _nullEntry.get();

  EXPECT_SOME_EQ(nullEntry, JSON::parse("{\"null_entry\" : null}"));
  EXPECT_SOME_NE(nullEntry, JSON::parse("{\"null_entry\" : 42}"));

  // String checks.
  Try<JSON::Value> _str = JSON::parse("{\"string\" : \"Hello World!\"}");
  ASSERT_SOME(_str);
  const JSON::Value str = _str.get();

  EXPECT_SOME_EQ(str, JSON::parse("{\"string\" : \"Hello World!\"}"));
  EXPECT_SOME_NE(str, JSON::parse("{\"string\" : \"Goodbye World!\"}"));
  EXPECT_SOME_NE(str, JSON::parse("{\"string\" : \"\"}"));
  EXPECT_SOME_NE(str, JSON::parse("{\"string\" : null}"));
  EXPECT_SOME_NE(str, JSON::parse("{\"string\" : 42}"));

  // Object's checks.
  Try<JSON::Value> _object = JSON::parse("{\"a\" : 1, \"b\" : 2}");
  ASSERT_SOME(_object);
  const JSON::Value object = _object.get();

  EXPECT_SOME_EQ(object, JSON::parse("{\"a\" : 1, \"b\" : 2}"));
  EXPECT_SOME_NE(object, JSON::parse("{\"a\" : 1, \"b\" : []}"));
  EXPECT_SOME_NE(object, JSON::parse("{\"a\" : 1}"));
  EXPECT_SOME_NE(object, JSON::parse("{}"));
}


// Test the containment of JSON objects where one is a JSON array.
TEST(JsonTest, ContainsArray)
{
  Try<JSON::Value> _array = JSON::parse("{\"array\" : [1, 2, 3]}");
  ASSERT_SOME(_array);
  const JSON::Value array = _array.get();

  Try<JSON::Value> arrayTest = JSON::parse("{\"array\" : [1, 2, 3]}");
  EXPECT_TRUE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{}");
  EXPECT_TRUE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{\"array\" : [3, 2, 1, 0]}");
  EXPECT_FALSE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{\"array\" : [1, 2, 3, 4]}");
  EXPECT_FALSE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{\"array\" : [3, 2, 1]}");
  EXPECT_FALSE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{\"array\" : [1, 2, 4]}");
  EXPECT_FALSE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{\"array\" : [1, 2]}");
  EXPECT_FALSE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{\"array\" : []}");
  EXPECT_FALSE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{\"array\" : null}");
  EXPECT_FALSE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{\"array\" : 42}");
  EXPECT_FALSE(array.contains(arrayTest.get()));

  arrayTest = JSON::parse("{\"array\" : \"A string\"}");
  EXPECT_FALSE(array.contains(arrayTest.get()));


  // Test arrays of doubles.
  Try<JSON::Value> _doubleArray =
    JSON::parse("{\"array_of_doubles\" : [1.0, -22.33, 99.987, 100]}");
  ASSERT_SOME(_doubleArray);
  const JSON::Value doubleArray = _doubleArray.get();

  Try<JSON::Value> doubleArrayTest =
    JSON::parse("{\"array_of_doubles\" : [1.0, -22.33, 99.987, 100]}");
  EXPECT_TRUE(doubleArray.contains(doubleArrayTest.get()));

  doubleArrayTest =
    JSON::parse("{\"array_of_doubles\" : [1.0, -22.33, 99.999, 100]}");
  EXPECT_FALSE(doubleArray.contains(doubleArrayTest.get()));

  doubleArrayTest = JSON::parse("{\"array_of_doubles\" : [1.0, -22.33, 100]}");
  EXPECT_FALSE(doubleArray.contains(doubleArrayTest.get()));


  // Test array of arrays.
  Try<JSON::Value> _arrayArray =
    JSON::parse("{\"array_of_arrays\" : [[1.0, -22.33], [1, 2]]}");
  ASSERT_SOME(_arrayArray);
  const JSON::Value arrayArray = _arrayArray.get();

  Try<JSON::Value> arrayArrayTest =
    JSON::parse("{\"array_of_arrays\" : [[1.0, -22.33], [1, 2]]}");
  EXPECT_TRUE(arrayArray.contains(arrayArrayTest.get()));

  arrayArrayTest =
    JSON::parse("{\"array_of_arrays\" : [[1.0, -22.33], [1, 3]]}");
  EXPECT_FALSE(arrayArray.contains(arrayArrayTest.get()));

  arrayArrayTest =
    JSON::parse("{\"array_of_arrays\" : [[1.0, -33.44], [1, 3]]}");
  EXPECT_FALSE(arrayArray.contains(arrayArrayTest.get()));

  arrayArrayTest =
    JSON::parse("{\"array_of_arrays\" : [[1.0, -22.33], [1]]}");
  EXPECT_FALSE(arrayArray.contains(arrayArrayTest.get()));
}


// Test the containment of JSON objects where one is a JSON boolean.
TEST(JsonTest, ContainsBoolean)
{
  Try<JSON::Value> _boolean = JSON::parse("{\"boolean\" : true}");
  ASSERT_SOME(_boolean);
  const JSON::Value boolean = _boolean.get();

  Try<JSON::Value> booleanTest = JSON::parse("{\"boolean\" : true}");
  EXPECT_TRUE(boolean.contains(booleanTest.get()));

  booleanTest = JSON::parse("{}");
  EXPECT_TRUE(boolean.contains(booleanTest.get()));

  booleanTest = JSON::parse("{\"boolean\" : false}");
  EXPECT_FALSE(boolean.contains(booleanTest.get()));

  booleanTest = JSON::parse("{\"boolean\" : null}");
  EXPECT_FALSE(boolean.contains(booleanTest.get()));

  booleanTest = JSON::parse("{\"boolean\" : 42}");
  EXPECT_FALSE(boolean.contains(booleanTest.get()));

  booleanTest = JSON::parse("{\"boolean\" : \"A string\"}");
  EXPECT_FALSE(boolean.contains(booleanTest.get()));
}


// Test the containment of JSON objects where one is a JSON null.
TEST(JsonTest, ContainsNull)
{
  Try<JSON::Value> _nullEntry = JSON::parse("{\"null_entry\" : null}");
  ASSERT_SOME(_nullEntry);
  const JSON::Value nullEntry = _nullEntry.get();

  Try<JSON::Value> nullEntryTest = JSON::parse("{\"null_entry\" : null}");
  EXPECT_TRUE(nullEntry.contains(nullEntryTest.get()));

  nullEntryTest = JSON::parse("{}");
  EXPECT_TRUE(nullEntry.contains(nullEntryTest.get()));

  nullEntryTest = JSON::parse("{\"null_entry\" : 42}");
  EXPECT_FALSE(nullEntry.contains(nullEntryTest.get()));

  nullEntryTest = JSON::parse("{\"null_entry\" : \"A string\"}");
  EXPECT_FALSE(nullEntry.contains(nullEntryTest.get()));
}


// Test the containment of JSON objects where one is a JSON string.
TEST(JsonTest, ContainsString)
{
  Try<JSON::Value> _str = JSON::parse("{\"string\" : \"Hello World!\"}");
  ASSERT_SOME(_str);
  const JSON::Value str = _str.get();

  Try<JSON::Value> strTest = JSON::parse("{\"string\" : \"Hello World!\"}");
  EXPECT_TRUE(str.contains(strTest.get()));

  strTest = JSON::parse("{}");
  EXPECT_TRUE(str.contains(strTest.get()));

  strTest = JSON::parse("{\"string\" : \"Goodbye World!\"}");
  EXPECT_FALSE(str.contains(strTest.get()));

  strTest = JSON::parse("{\"string\" : \"\"}");
  EXPECT_FALSE(str.contains(strTest.get()));

  strTest = JSON::parse("{\"string\" : null}");
  EXPECT_FALSE(str.contains(strTest.get()));

  strTest = JSON::parse("{\"string\" : 42}");
  EXPECT_FALSE(str.contains(strTest.get()));

  strTest = JSON::parse("{\"string\" : [42]}");
  EXPECT_FALSE(str.contains(strTest.get()));
}


// Test the containment of JSON objects to JSON objects.
TEST(JsonTest, ContainsObject)
{
  Try<JSON::Value> _object = JSON::parse("{\"a\" : 1, \"b\" : 2}");
  ASSERT_SOME(_object);
  const JSON::Value object = _object.get();

  Try<JSON::Value> objectTest = JSON::parse("{\"a\" : 1, \"b\" : 2}");
  EXPECT_TRUE(object.contains(objectTest.get()));

  objectTest = JSON::parse("{\"a\" : 1}");
  EXPECT_TRUE(object.contains(objectTest.get()));

  objectTest = JSON::parse("{\"b\" : 2}");
  EXPECT_TRUE(object.contains(objectTest.get()));

  objectTest = JSON::parse("{}");
  EXPECT_TRUE(object.contains(objectTest.get()));

  objectTest = JSON::parse("{\"a\" : 2}");
  EXPECT_FALSE(object.contains(objectTest.get()));

  objectTest = JSON::parse("{\"a\" : 1, \"b\" : []}");
  EXPECT_FALSE(object.contains(objectTest.get()));


  // Array of objects checks.
  Try<JSON::Value> _objectArray = JSON::parse(
      "{"
      "  \"objectarray\" : ["
      "    {\"a\" : 1, \"b\" : 2},"
      "    {\"c\" : 3, \"d\" : 4}"
      "  ]"
      "}").get();
  ASSERT_SOME(_objectArray);
  const JSON::Value objectArray = _objectArray.get();

  Try<JSON::Value> objectArrayTest = objectArray;
  EXPECT_TRUE(objectArray.contains(objectArrayTest.get()));

  objectArrayTest = JSON::parse("{}");
  EXPECT_TRUE(objectArray.contains(objectArrayTest.get()));

  objectArrayTest = JSON::parse(
      "{"
      "  \"objectarray\" : ["
      "    {\"a\" : 1, \"b\" : 2},"
      "    {\"c\" : 3}"
      "  ]"
      "}");
  EXPECT_TRUE(objectArray.contains(objectArrayTest.get()));

  objectArrayTest = JSON::parse(
      "{"
      "  \"objectarray\" : ["
      "    {\"a\" : 1},"
      "    {}"
      "  ]"
      "}");
  EXPECT_TRUE(objectArray.contains(objectArrayTest.get()));

  objectArrayTest = JSON::parse(
      "{"
      "  \"objectarray\" : ["
      "    {},"
      "    {}"
      "  ]"
      "}");
  EXPECT_TRUE(objectArray.contains(objectArrayTest.get()));

  objectArrayTest = JSON::parse(
      "{"
      "  \"objectarray\" : ["
      "    {\"c\" : 3, \"d\" : 4},"
      "    {\"a\" : 1, \"b\" : 2}"
      "  ]"
      "}");
  EXPECT_FALSE(objectArray.contains(objectArrayTest.get()));

  objectArrayTest = JSON::parse(
      "{"
      "  \"objectarray\" : ["
      "    {\"e\" : 5},"
      "    {}"
      "  ]"
      "}");
  EXPECT_FALSE(objectArray.contains(objectArrayTest.get()));

  objectArrayTest = JSON::parse(
      "{"
      "  \"objectarray\" : []"
      "}");
  EXPECT_FALSE(objectArray.contains(objectArrayTest.get()));

  objectArrayTest = JSON::parse(
      "{"
      "  \"objectarray\" : ["
      "    {},"
      "    {},"
      "    {}"
      "  ]"
      "}");
  EXPECT_FALSE(objectArray.contains(objectArrayTest.get()));


  // Tests on nested objects.
  Try<JSON::Value> _nested = JSON::parse(
      "{"
      "  \"object\" : {"
      "    \"a\" : 1,"
      "    \"b\" : 2"
      "  }"
      "}");
  ASSERT_SOME(_nested);
  const JSON::Value nested = _nested.get();

  Try<JSON::Value> nestedTest = nested;
  EXPECT_TRUE(nested.contains(nestedTest.get()));

  nestedTest = JSON::parse("{}");
  EXPECT_TRUE(nested.contains(nestedTest.get()));

  nestedTest = JSON::parse("{\"object\" : {}}");
  EXPECT_TRUE(nested.contains(nestedTest.get()));

  nestedTest = JSON::parse("{\"object\" : {\"a\" : 1}}");
  EXPECT_TRUE(nested.contains(nestedTest.get()));

  nestedTest = JSON::parse("{\"object\" : {\"c\" : 1}}");
  EXPECT_FALSE(nested.contains(nestedTest.get()));

  nestedTest = JSON::parse(
      "{"
      "  \"object\" : {"
      "    \"a\" : 1,"
      "    \"b\" : 2,"
      "    \"c\" : 3"
      "  }"
      "}");
  EXPECT_FALSE(nested.contains(nestedTest.get()));
}
