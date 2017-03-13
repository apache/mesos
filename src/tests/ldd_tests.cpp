// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>
#include <vector>

#include <process/owned.hpp>

#include <stout/elf.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "linux/ldd.hpp"

#include "tests/mesos.hpp"

using process::Owned;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

TEST(Ldd, BinSh)
{
  Try<vector<ldcache::Entry>> cache = ldcache::parse();
  ASSERT_SOME(cache);

  Try<hashset<string>> dependencies = ldd("/bin/sh", cache.get());
  ASSERT_SOME(dependencies);

  EXPECT_FALSE(dependencies->contains("/bin/sh"));

  auto libc = std::find_if(
      dependencies->begin(),
      dependencies->end(),
      [](const string& dependency) {
        // On most Linux systems, libc would be in libc.so.6, but
        // checking the unversioned prefix is robust and is enough
        // to know that ldd() worked.
        string basename = Path(dependency).basename();
        return strings::startsWith(basename, "libc.so");
      });

  EXPECT_TRUE(libc != dependencies->end());
}


TEST(Ldd, EmptyCache)
{
  vector<ldcache::Entry> cache;

  Try<hashset<string>> dependencies = ldd("/bin/sh", cache);
  ASSERT_ERROR(dependencies);
}


TEST(Ldd, MissingFile)
{
  Try<vector<ldcache::Entry>> cache = ldcache::parse();
  ASSERT_SOME(cache);

  Try<hashset<string>> dependencies =
    ldd("/this/path/is/not/here", cache.get());
  ASSERT_ERROR(dependencies);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
