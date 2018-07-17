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

#ifndef __MEMORY_TEST_HELPER_HPP__
#define __MEMORY_TEST_HELPER_HPP__

#include <process/subprocess.hpp>

#include <stout/bytes.hpp>
#include <stout/option.hpp>
#include <stout/subcommand.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace tests {

// The abstraction for controlling the memory usage of a subprocess.
// TODO(chzhcn): Currently, this helper is only supposed to be used by
// one thread. Consider making it thread safe.
class MemoryTestHelper : public Subcommand
{
public:
  static const char NAME[];

  MemoryTestHelper() : Subcommand(NAME) {}
  ~MemoryTestHelper() override;

  // Spawns a subprocess.
  // TODO(chzhcn): Consider returning a future instead of blocking.
  Try<Nothing> spawn();

  // Kill and reap the subprocess if exists.
  // TODO(chzhcn): Consider returning a future instead of blocking.
  void cleanup();

  // Returns the pid of the subprocess.
  Try<pid_t> pid();

  // Allocate and lock specified page-aligned anonymous memory (RSS)
  // in the subprocess. It uses mlock and memset to make sure
  // allocated memory is mapped.
  // TODO(chzhcn): Consider returning a future instead of blocking.
  Try<Nothing> increaseRSS(const Bytes& size);

  // This function attempts to generate requested size of page cache
  // in the subprocess by using a small buffer and writing it to disk
  // multiple times.
  // TODO(chzhcn): Consider returning a future instead of blocking.
  Try<Nothing> increasePageCache(const Bytes& size = Megabytes(1));

protected:
  // The main function of the subprocess. It runs in a loop and
  // executes commands passed from stdin.
  int execute() override;

private:
  Try<Nothing> requestAndWait(const std::string& request);

  Option<process::Subprocess> s;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __MEMORY_TEST_HELPER_HPP__
