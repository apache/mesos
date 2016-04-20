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

#include <string>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

using std::string;


Error error1()
{
  return Error("Failed to ...");
}


Try<string> error2()
{
  return Error("Failed to ...");
}


Try<string> error3(const Try<string>& t)
{
  return t;
}


Result<string> error4()
{
  return Error("Failed to ...");
}


Result<string> error5(const Result<string>& r)
{
  return r;
}


TEST(ErrorTest, Test)
{
  Try<string> t = error1();
  EXPECT_ERROR(t);
  t = error2();
  EXPECT_ERROR(t);
  t = error3(error1());
  EXPECT_ERROR(t);

  Result<string> r = error1();
  EXPECT_ERROR(r);
  r = error4();
  EXPECT_ERROR(r);
  r = error5(error1());
  EXPECT_ERROR(r);
}
