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

#ifndef __EXAMPLES_FLAGS_HPP__
#define __EXAMPLES_FLAGS_HPP__

#include <string>

#include <stout/flags.hpp>
#include <stout/option.hpp>

#include "logging/flags.hpp"

namespace mesos {
namespace internal {
namespace examples {

class Flags : public virtual mesos::internal::logging::Flags
{
public:
  Flags()
  {
    add(&Flags::master,
        "master",
        "The master to connect to. May be one of:\n"
        "  `host:port`\n"
        "  `master@host:port` (PID of the master)\n"
        "  `zk://host1:port1,host2:port2,.../path`\n"
        "  `zk://username:password@host1:port1,host2:port2,.../path`\n"
        "  `file://path/to/file` (where file contains one of the above)\n"
        "  `local` (launches mesos-local)");

    add(&Flags::authenticate,
        "authenticate",
        "Set to 'true' to enable framework authentication.",
        false);

    add(&Flags::principal,
        "principal",
        "The principal used to identify this framework.",
        "test");

    add(&Flags::secret,
        "secret",
        "The secret used to authenticate this framework.\n"
        "If the value starts with '/' or 'file://' it will be parsed as the\n"
        "path to a file containing the secret. Otherwise the string value is\n"
        "treated as the secret.");

    add(&Flags::checkpoint,
        "checkpoint",
        "Whether this framework should be checkpointed.",
        false);

    add(&Flags::role,
        "role",
        "Role to use when registering.",
        "*");
  }

  std::string master;
  bool authenticate;
  std::string principal;
  Option<std::string> secret;
  bool checkpoint;
  std::string role;
};


} // namespace examples {
} // namespace internal {
} // namespace mesos {

#endif // __EXAMPLES_FLAGS_HPP__
