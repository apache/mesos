// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <stout/jsonify.hpp>

using std::map;
using std::multimap;
using std::set;
using std::string;
using std::vector;

// Tests that booleans are jsonified correctly.
TEST(JsonifyTest, Boolean)
{
  EXPECT_EQ("true", string(jsonify(true)));
  EXPECT_EQ("false", string(jsonify(false)));
}


// Tests that numbers are jsonified correctly.
TEST(JsonifyTest, Number)
{
  // Test whole numbers (as doubles).
  EXPECT_EQ("0", string(jsonify(0.0)));
  EXPECT_EQ("1", string(jsonify(1.0)));

  // Negative.
  EXPECT_EQ("-1", string(jsonify(-1.0)));

  // Test integers.
  EXPECT_EQ("0", string(jsonify(0)));
  EXPECT_EQ("2", string(jsonify(2)));
  EXPECT_EQ("-2", string(jsonify(-2)));

  // Expect at least 15 digits of precision.
  EXPECT_EQ("1234567890.12345", string(jsonify(1234567890.12345)));
}


// Tests that strings are jsonified correctly, including escaping.
TEST(JsonifyTest, String)
{
  EXPECT_EQ("\"hello world!\"", string(jsonify("hello world!")));
  EXPECT_EQ(
      "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\\u0019 !#[]\\u007f\xFF\"",
      string(jsonify(string("\"\\/\b\f\n\r\t\x00\x19 !#[]\x7F\xFF", 17))));
}


namespace store {

// A simple object consisting of primitive types.
struct Name
{
  string first;
  string last;
};


// A simple object consisting of primitive types as well as a sub-object.
struct Customer
{
  Name name;
  int age;
};


// `json` overload for `Name`.
void json(JSON::ObjectWriter* writer, const Name& name)
{
  writer->field("first_name", name.first);
  writer->field("last_name", name.last);
}


// `json` overload for `Customer`.
void json(JSON::ObjectWriter* writer, const Customer& customer)
{
  json(writer, customer.name);  // Composition of `json` functions!
  writer->field("age", customer.age);
}

} // namespace store {


// Tests that objects are jsonified correctly.
TEST(JsonifyTest, Object)
{
  store::Name name{"michael", "park"};
  EXPECT_EQ(
      "{\"first_name\":\"michael\",\"last_name\":\"park\"}",
      string(jsonify(name)));

  store::Customer customer{name, 25};
  EXPECT_EQ(
      "{\"first_name\":\"michael\",\"last_name\":\"park\",\"age\":25}",
      string(jsonify(customer)));
}


// Tests that iterable types are jsonified as array correctly.
TEST(JsonifyTest, Array)
{
  bool booleans[] = {true, true, false};
  EXPECT_EQ("[true,true,false]", string(jsonify(booleans)));

  vector<int> numbers = {1, 2, 3};
  EXPECT_EQ("[1,2,3]", string(jsonify(numbers)));

  set<string> strings = {"there", "hello"};
  EXPECT_EQ("[\"hello\",\"there\"]", string(jsonify(strings)));

  vector<set<int>> numbers_list = {{1, 2, 3}, {1, 1, 1}};
  EXPECT_EQ("[[1,2,3],[1]]", string(jsonify(numbers_list)));

  vector<store::Customer> names = {{{"michael", "park"}, 25}};
  EXPECT_EQ(
      "[{\"first_name\":\"michael\",\"last_name\":\"park\",\"age\":25}]",
      string(jsonify(names)));
}


// Tests that dictionary types of primitive a member typedef `mapped_type` are
// considered dictionaries and therefore are jsonified as objects correctly.
TEST(JsonifyTest, Dictionary)
{
  map<string, bool> booleans = {{"x", true}, {"y", false}};
  EXPECT_EQ("{\"x\":true,\"y\":false}", string(jsonify(booleans)));

  map<string, map<string, int>> nested_numbers = {{"foo", {{"x", 1}}}};
  EXPECT_EQ("{\"foo\":{\"x\":1}}", string(jsonify(nested_numbers)));

  multimap<string, store::Name> names = {
    {"kobe", {"kobe", "bryant"}},
    {"michael", {"michael", "jordan"}},
    {"michael", {"michael", "park"}}
  };

  string expected = strings::remove(
      "{"
      "  \"kobe\":{\"first_name\":\"kobe\",\"last_name\":\"bryant\"},"
      "  \"michael\":{\"first_name\":\"michael\",\"last_name\":\"jordan\"},"
      "  \"michael\":{\"first_name\":\"michael\",\"last_name\":\"park\"}"
      "}",
      " ");

  EXPECT_EQ(expected, string(jsonify(names)));
}
