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

#ifndef __TESTS_FLAGS_HPP__
#define __TESTS_FLAGS_HPP__

#include <string>

#include <mesos/module/module.hpp>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>

#include "logging/flags.hpp"

namespace mesos {
namespace internal {
namespace tests {

class Flags : public virtual logging::Flags
{
public:
  Flags();

  bool verbose;
  bool benchmark;
  std::string source_dir;
  std::string build_dir;
  std::string docker;
  std::string docker_socket;
  Option<Modules> modules;
  Option<std::string> modulesDir;
  Option<std::string> isolation;
  std::string authenticators;
  Duration test_await_timeout;
};

// Global flags for running the tests.
extern Flags flags;

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_FLAGS_HPP__
