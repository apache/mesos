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
#include <stout/try.hpp>

#include "linux/ldcache.hpp"

#include "tests/mesos.hpp"

using process::Owned;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

// Test to make sure we are able to read in and parse the ld.so.cache
// file on a Linux machine.
TEST(LdcacheTest, Parse)
{
  Try<vector<ldcache::Entry>> cache = ldcache::parse();
  ASSERT_SOME(cache);
  ASSERT_GT(cache->size(), 1u);

  foreach (const ldcache::Entry& entry, cache.get()) {
    Try<elf::File*> load = elf::File::load(entry.path);
    ASSERT_SOME(load);

    Owned<elf::File> file(load.get());

    Try<elf::Class> c = file->get_class();
    ASSERT_SOME(c);
    ASSERT_TRUE(c.get() == elf::CLASS32 || c.get() == elf::CLASS64);

    Try<vector<string>> soname =
      file->get_dynamic_strings(elf::DynamicTag::SONAME);
    ASSERT_SOME(soname);
    ASSERT_LE(soname->size(), 1u);

    Try<vector<string>> needed =
      file->get_dynamic_strings(elf::DynamicTag::NEEDED);
    ASSERT_SOME(needed);
    ASSERT_LE(needed->size(), cache->size());

    Result<Version> abiVersion = file->get_abi_version();
    if (!abiVersion.isNone()) {
      ASSERT_SOME(abiVersion);
    }
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
