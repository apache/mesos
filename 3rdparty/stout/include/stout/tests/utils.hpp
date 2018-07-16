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

#ifndef __STOUT_TESTS_UTILS_HPP__
#define __STOUT_TESTS_UTILS_HPP__

#include <string>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include <stout/os/chdir.hpp>
#include <stout/os/getcwd.hpp>
#include <stout/os/mkdtemp.hpp>
#include <stout/os/realpath.hpp>
#include <stout/os/rmdir.hpp>

#if __FreeBSD__
#include <stout/os/sysctl.hpp>
#endif

template <typename T>
class MixinTemporaryDirectoryTest : public T
{
protected:
  void SetUp() override
  {
    T::SetUp();

    ASSERT_SOME(SetUpMixin());
  }

  void TearDown() override
  {
    ASSERT_SOME(TearDownMixin());

    T::TearDown();
  }

  Try<Nothing> SetUpMixin()
  {
    // Save the current working directory.
    cwd = os::getcwd();

    // Create a temporary directory for the test.
    Try<std::string> directory = os::mkdtemp();

    if (directory.isError()) {
      return Error("Failed to mkdtemp: " + directory.error());
    }

    // We get the `realpath` of the temporary directory because some
    // platforms, like macOS, symlink `/tmp` to `/private/var`, but
    // return the symlink name when creating temporary directories.
    // This is problematic because a lot of tests compare the
    // `realpath` of a temporary file.
    Result<std::string> realpath = os::realpath(directory.get());

    if (realpath.isError()) {
      return Error("Failed to get realpath of '" + directory.get() + "'"
                   ": " + realpath.error());
    } else if (realpath.isNone()) {
      return Error("Failed to get realpath of '" + directory.get() + "'"
                   ": No such directory");
    }

    sandbox = realpath.get();

    // Run the test out of the temporary directory we created.
    Try<Nothing> chdir = os::chdir(sandbox.get());

    if (chdir.isError()) {
      return Error("Failed to chdir into '" + sandbox.get() + "'"
                   ": " + chdir.error());
    }

    return Nothing();
  }

  Try<Nothing> TearDownMixin()
  {
    // Return to previous working directory and cleanup the sandbox.
    Try<Nothing> chdir = os::chdir(cwd);

    if (chdir.isError()) {
      return Error("Failed to chdir into '" + cwd + "': " + chdir.error());
    }

    if (sandbox.isSome()) {
      Try<Nothing> rmdir = os::rmdir(sandbox.get());
      if (rmdir.isError()) {
        return Error("Failed to rmdir '" + sandbox.get() + "'"
                     ": " + rmdir.error());
      }
    }

    return Nothing();
  }

  // A temporary directory for test purposes.
  // Not to be confused with the "sandbox" that tasks are run in.
  Option<std::string> sandbox;

private:
  std::string cwd;
};


class TemporaryDirectoryTest
  : public MixinTemporaryDirectoryTest<::testing::Test> {};


#ifdef __FreeBSD__
inline bool isJailed() {
  int mib[4];
  size_t len = 4;
  ::sysctlnametomib("security.jail.jailed", mib, &len);
  Try<int> jailed = os::sysctl(mib[0], mib[1], mib[2]).integer();
  if (jailed.isSome()) {
      return jailed.get() == 1;
  }

  return false;
}
#endif

#endif // __STOUT_TESTS_UTILS_HPP__
