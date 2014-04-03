/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __TESTS_FLAGS_HPP__
#define __TESTS_FLAGS_HPP__

#include <string>

#include <stout/check.hpp>
#include <stout/flags.hpp>
#include <stout/os.hpp>

#include "logging/logging.hpp"

namespace mesos {
namespace internal {
namespace tests {

class Flags : public logging::Flags
{
public:
  Flags()
  {
    // We log to stderr by default, but when running tests we'd prefer
    // less junk to fly by, so force one to specify the verbosity.
    add(&Flags::verbose,
        "verbose",
        "Log all severity levels to stderr",
        false);

    add(&Flags::benchmark,
        "benchmark",
        "Run the benchmark tests (and skip other tests)",
        false);

    // We determine the defaults for 'source_dir' and 'build_dir' from
    // preprocessor definitions (at the time this comment was written
    // these were set via '-DSOURCE_DIR=...' and '-DBUILD_DIR=...' in
    // src/Makefile.am).
    Result<std::string> path = os::realpath(SOURCE_DIR);
    CHECK_SOME(path);
    add(&Flags::source_dir,
        "source_dir",
        "Where to find the source directory",
        path.get());

    path = os::realpath(BUILD_DIR);
    CHECK_SOME(path);
    add(&Flags::build_dir,
        "build_dir",
        "Where to find the build directory",
        path.get());
  }

  bool verbose;
  bool benchmark;
  std::string source_dir;
  std::string build_dir;
};

// Global flags for running the tests.
extern Flags flags;

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_FLAGS_HPP__
