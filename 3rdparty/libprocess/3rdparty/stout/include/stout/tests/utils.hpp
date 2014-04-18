/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __STOUT_TESTS_UTILS_HPP__
#define __STOUT_TESTS_UTILS_HPP__

#include <gtest/gtest.h>

#include <string>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

class TemporaryDirectoryTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    // Save the current working directory.
    cwd = os::getcwd();

    // Create a temporary directory for the test.
    Try<std::string> directory = os::mkdtemp();

    ASSERT_SOME(directory) << "Failed to mkdtemp";

    sandbox = directory.get();

    // Run the test out of the temporary directory we created.
    ASSERT_SOME(os::chdir(sandbox.get()))
      << "Failed to chdir into '" << sandbox.get() << "'";
  }

  virtual void TearDown()
  {
    // Return to previous working directory and cleanup the sandbox.
    ASSERT_SOME(os::chdir(cwd));

    if (sandbox.isSome()) {
      ASSERT_SOME(os::rmdir(sandbox.get()));
    }
  }

private:
  std::string cwd;
  Option<std::string> sandbox;
};

#endif // __STOUT_TESTS_UTILS_HPP__
