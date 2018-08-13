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

#ifndef __SCHED_FLAGS_HPP__
#define __SCHED_FLAGS_HPP__

#include <stout/flags.hpp>

#include "common/parse.hpp"

#include "logging/flags.hpp"

#include "messages/messages.hpp"

#include "sched/constants.hpp"

namespace mesos {
namespace internal {
namespace scheduler {

class Flags : public virtual logging::Flags
{
public:
  Flags()
  {
    add(&Flags::registration_backoff_factor,
        "registration_backoff_factor",
        "Scheduler driver (re-)registration retries are exponentially backed\n"
        "off based on 'b', the registration backoff factor (e.g., 1st retry\n"
        "uses a random value between [0, b], 2nd retry between [0, b * 2^1],\n"
        "3rd retry between [0, b * 2^2]...) up to a maximum of (framework\n"
        "failover timeout/10, if failover timeout is specified) or " +
        stringify(REGISTRATION_RETRY_INTERVAL_MAX) + ", whichever is smaller",
        DEFAULT_REGISTRATION_BACKOFF_FACTOR);

    // This help message for --modules flag is the same for
    // {master,slave,sched,tests}/flags.[ch]pp and should always be kept in
    // in sync.
    // TODO(karya): Remove the JSON example and add reference to the
    // doc file explaining the --modules flag.
    add(&Flags::modules,
        "modules",
        "List of modules to be loaded and be available to the internal\n"
        "subsystems.\n"
        "\n"
        "Use --modules=filepath to specify the list of modules via a\n"
        "file containing a JSON formatted string. 'filepath' can be\n"
        "of the form 'file:///path/to/file' or '/path/to/file'.\n"
        "\n"
        "Use --modules=\"{...}\" to specify the list of modules inline.\n"
        "\n"
        "Example:\n"
        "{\n"
        "  \"libraries\": [\n"
        "    {\n"
        "      \"file\": \"/path/to/libfoo.so\",\n"
        "      \"modules\": [\n"
        "        {\n"
        "          \"name\": \"org_apache_mesos_bar\",\n"
        "          \"parameters\": [\n"
        "            {\n"
        "              \"key\": \"X\",\n"
        "              \"value\": \"Y\"\n"
        "            }\n"
        "          ]\n"
        "        },\n"
        "        {\n"
        "          \"name\": \"org_apache_mesos_baz\"\n"
        "        }\n"
        "      ]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"qux\",\n"
        "      \"modules\": [\n"
        "        {\n"
        "          \"name\": \"org_apache_mesos_norf\"\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}");

    // This help message for --modules_dir flag is the same for
    // {master,slave,sched,tests}/flags.[ch]pp and should always be kept in
    // sync.
    add(&Flags::modulesDir,
        "modules_dir",
        "Directory path of the module manifest files.\n"
        "The manifest files are processed in alphabetical order.\n"
        "(See --modules for more information on module manifest files).\n"
        "Cannot be used in conjunction with --modules.\n");

    add(&Flags::authenticatee,
        "authenticatee",
        "Authenticatee implementation to use when authenticating against the\n"
        "master. Use the default '" + std::string(DEFAULT_AUTHENTICATEE) + "'\n"
        "or load an alternate authenticatee module using MESOS_MODULES.",
        DEFAULT_AUTHENTICATEE);

    add(&Flags::authentication_backoff_factor,
        "authentication_backoff_factor",
        "The scheduler will time out its authentication with the master based\n"
        "on exponential backoff. The timeout will be randomly chosen within\n"
        "the range `[min, min + factor*2^n]` where `n` is the number of\n"
        "failed attempts. To tune these parameters, set the\n"
        "`--authentication_timeout_[min|max|factor]` flags.\n",
        DEFAULT_AUTHENTICATION_BACKOFF_FACTOR);

    add(&Flags::authentication_timeout_min,
        "authentication_timeout_min",
        flags::DeprecatedName("authentication_timeout"),
        "The minimum amount of time the scheduler waits before retrying\n"
        "authenticating with the master. See `authentication_backoff_factor`\n"
        "for more details. NOTE: since authentication retry cancels the\n"
        "previous authentication request, one should consider what is the\n"
        "normal authentication delay when setting this flag to prevent\n"
        "premature retry",
        DEFAULT_AUTHENTICATION_TIMEOUT_MIN);

    add(&Flags::authentication_timeout_max,
      "authentication_timeout_max",
      "The maximum amount of time the scheduler waits before retrying\n"
      "authenticating with the master. See `authentication_backoff_factor`\n"
      "for more details",
      DEFAULT_AUTHENTICATION_TIMEOUT_MAX);
  }

  Duration authentication_backoff_factor;
  Duration registration_backoff_factor;
  Option<Modules> modules;
  Option<std::string> modulesDir;
  std::string authenticatee;
  Duration authentication_timeout_min;
  Duration authentication_timeout_max;
};

} // namespace scheduler {
} // namespace internal {
} // namespace mesos {

#endif // __SCHED_FLAGS_HPP__
