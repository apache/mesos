// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <list>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>
#include <stout/numify.hpp>

struct OnlyMoveable
{
  OnlyMoveable() : i(-1) {}
  OnlyMoveable(int i) : i(i) {}

  OnlyMoveable(OnlyMoveable&& that)
  {
    *this = std::move(that);
  }

  OnlyMoveable(const OnlyMoveable&) = delete;

  OnlyMoveable& operator=(OnlyMoveable&& that)
  {
    i = that.i;
    j = that.j;
    that.valid = false;
    return *this;
  }

  OnlyMoveable& operator=(const OnlyMoveable&) = delete;

  int i;
  int j = 0;
  bool valid = true;
};


std::vector<std::string> function()
{
  return {"1", "2", "3"};
}


TEST(LambdaTest, Map)
{
  std::vector<int> expected = {1, 2, 3};

  EXPECT_EQ(
      expected,
      lambda::map(
          [](std::string s) {
            return numify<int>(s).get();
          },
          std::vector<std::string>{"1", "2", "3"}));

  EXPECT_EQ(
      expected,
      lambda::map(
          [](const std::string& s) {
            return numify<int>(s).get();
          },
          std::vector<std::string>{"1", "2", "3"}));

  EXPECT_EQ(
      expected,
      lambda::map(
          [](std::string&& s) {
            return numify<int>(s).get();
          },
          std::vector<std::string>{"1", "2", "3"}));

  std::vector<std::string> concat = {"11", "22", "33"};

  EXPECT_EQ(
      concat,
      lambda::map(
          [](std::string&& s) {
            return s + s;
          },
          function()));

  std::vector<OnlyMoveable> v;
  v.emplace_back(1);
  v.emplace_back(2);

  std::vector<OnlyMoveable> result = lambda::map(
      [](OnlyMoveable&& o) {
        o.j = o.i;
        return std::move(o);
      },
      std::move(v));

  for (const OnlyMoveable& o : result) {
    EXPECT_EQ(o.i, o.j);
  }
}


TEST(LambdaTest, Zip)
{
  std::vector<int> ints = {1, 2, 3, 4, 5, 6, 7, 8};
  std::list<std::string> strings = {"hello", "world"};

  hashmap<int, std::string> zip1 = lambda::zip(ints, strings);

  ASSERT_EQ(2u, zip1.size());
  EXPECT_EQ(std::string("hello"), zip1[1]);
  EXPECT_EQ(std::string("world"), zip1[2]);

  ints = {1, 2};
  strings = {"hello", "world", "!"};

  std::vector<std::pair<int, std::string>> zip2 =
    lambda::zipto<std::vector>(ints, strings);

  ASSERT_EQ(2u, zip2.size());
  EXPECT_EQ(std::make_pair(1, std::string("hello")), zip2.at(0));
  EXPECT_EQ(std::make_pair(2, std::string("world")), zip2.at(1));
}


namespace {

template <typename F, typename ...Args>
auto callable(F&& f, Args&&... args)
  -> decltype(std::forward<F>(f)(std::forward<Args>(args)...),
              void(),
              std::true_type());


template <typename F>
std::false_type callable(F&& f, ...);


// Compile-time check that f cannot be called with specified arguments.
// This is implemented by defining two callable function overloads and
// differentiating on return type. The first overload is selected only
// when call expression is valid, and it has return type of std::true_type,
// while second overload is selected for everything else.
template <typename F, typename ...Args>
void EXPECT_CALL_INVALID(F&& f, Args&&... args)
{
  static_assert(
      !decltype(
          callable(std::forward<F>(f), std::forward<Args>(args)...))::value,
          "call expression is expected to be invalid");
}

} // namespace {



namespace {

int returnIntNoParams()
{
  return 8;
}


void returnVoidStringParam(std::string s) {}


void returnVoidStringCRefParam(const std::string& s) {}


int returnIntOnlyMovableParam(OnlyMoveable o)
{
  EXPECT_TRUE(o.valid);
  return 1;
}

} // namespace {


// This is mostly a compile time test of lambda::partial,
// verifying that it works for different types of expressions.
TEST(PartialTest, Test)
{
  // standalone functions
  auto p1 = lambda::partial(returnIntNoParams);
  int p1r1 = p1();
  int p1r2 = std::move(p1)();
  EXPECT_EQ(p1r1, p1r2);

  auto p2 = lambda::partial(returnVoidStringParam, "");
  p2();
  std::move(p2)();

  auto p3 = lambda::partial(returnVoidStringParam, lambda::_1);
  p3("");
  std::move(p3)("");

  auto p4 = lambda::partial(&returnVoidStringCRefParam, lambda::_1);
  p4("");
  std::move(p4)("");

  auto p5 = lambda::partial(&returnIntOnlyMovableParam, lambda::_1);
  p5(OnlyMoveable());
  p5(10);
  std::move(p5)(OnlyMoveable());

  auto p6 = lambda::partial(&returnIntOnlyMovableParam, OnlyMoveable());
  EXPECT_CALL_INVALID(p6);
  std::move(p6)();

  // lambdas
  auto l1 = [](const OnlyMoveable& m) { EXPECT_TRUE(m.valid); };
  auto pl1 = lambda::partial(l1, OnlyMoveable());
  pl1();
  pl1();
  std::move(pl1)();

  auto pl2 = lambda::partial([](OnlyMoveable&& m) { EXPECT_TRUE(m.valid); },
      lambda::_1);
  pl2(OnlyMoveable());
  pl2(OnlyMoveable());
  std::move(pl2)(OnlyMoveable());

  auto pl3 = lambda::partial([](OnlyMoveable&& m) { EXPECT_TRUE(m.valid); },
      OnlyMoveable());
  EXPECT_CALL_INVALID(pl3);
  std::move(pl3)();

  // member functions
  struct Object
  {
    int method() { return 0; };
  };

  auto mp1 = lambda::partial(&Object::method, lambda::_1);
  mp1(Object());
  std::move(mp1)(Object());

  auto mp2 = lambda::partial(&Object::method, Object());
  mp2();
  std::move(mp2)();
}
