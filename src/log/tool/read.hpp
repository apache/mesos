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

#ifndef __LOG_TOOL_READ_HPP__
#define __LOG_TOOL_READ_HPP__

#include <stdint.h>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>

#include "log/tool.hpp"

#include "logging/flags.hpp"

namespace mesos {
namespace internal {
namespace log {
namespace tool {

class Read : public Tool
{
public:
  class Flags : public virtual logging::Flags
  {
  public:
    Flags();

    Option<std::string> path;
    Option<uint64_t> from;
    Option<uint64_t> to;
    Option<Duration> timeout;
    bool help;
  };

  std::string name() const override { return "read"; }
  Try<Nothing> execute(int argc = 0, char** argv = nullptr) override;

  // Users can change the default configuration by setting this flags.
  Flags flags;
};

} // namespace tool {
} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_TOOL_READ_HPP__
