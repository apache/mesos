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

#ifndef __LOG_TOOL_HPP__
#define __LOG_TOOL_HPP__

#include <string>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace log {
namespace tool {

// Represents a tool for processing a log file.
class Tool
{
public:
  virtual ~Tool() {}

  virtual std::string name() const = 0;

  // Executes the tool. The tool can be configured by passing in
  // command line arguments. If command line arguments are not
  // specified, the default configuration will be used.
  virtual Try<Nothing> execute(int argc, char** argv) = 0;
};

} // namespace tool {
} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_TOOL_HPP__
