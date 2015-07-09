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

#include <string>

#include <gtest/gtest.h>

#include <stout/thread.hpp>

TEST(ThreadTest, Local)
{
  ThreadLocal<std::string>* _s_ = new ThreadLocal<std::string>();

  std::string* s = new std::string();

  ASSERT_TRUE(*(_s_) == NULL);

  (*_s_) = s;

  ASSERT_TRUE(*(_s_) == s);
  ASSERT_FALSE(*(_s_) == NULL);

  (*_s_) = NULL;

  ASSERT_TRUE(*(_s_) == NULL);

  delete s;
  delete _s_;
}
