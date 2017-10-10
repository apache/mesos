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

#ifndef __SCHEDULER_FLAGS_HPP__
#define __SCHEDULER_FLAGS_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include "common/parse.hpp"

#include "local/flags.hpp"

#include "scheduler/constants.hpp"

namespace mesos {
namespace v1 {
namespace scheduler {

class Flags : public virtual mesos::internal::local::Flags
{
public:
  Flags()
  {
    add(&Flags::connectionDelayMax,
        "connection_delay_max",
        "The maximum amount of time to wait before trying to initiate a\n"
        "connection with the master. The library waits for a random amount of\n"
        "time between [0, b], where `b = connection_delay_max` before\n"
        "initiating a (re-)connection attempt with the master.",
        DEFAULT_CONNECTION_DELAY_MAX);

    add(&Flags::httpAuthenticatee,
        "http_authenticatee",
        "HTTP authenticatee implementation to use when authenticating against\n"
        "the master. Use the default '" +
          std::string(mesos::internal::DEFAULT_BASIC_HTTP_AUTHENTICATEE) +
          "' or load an alternate\n"
        "authenticatee module using MESOS_MODULES.",
        mesos::internal::DEFAULT_BASIC_HTTP_AUTHENTICATEE);

    // This help message for --modules flag is the same for
    // {master,slave,sched,tests}/flags.[ch]pp and should always be kept
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
    // {master,slave,sched,tests}/flags.[ch]pp and should always be kept
    // in sync.
    add(&Flags::modulesDir,
        "modules_dir",
        "Directory path of the module manifest files.\n"
        "The manifest files are processed in alphabetical order.\n"
        "(See --modules for more information on module manifest files).\n"
        "Cannot be used in conjunction with --modules.\n");
  }

  Duration connectionDelayMax;
  Option<Modules> modules;
  Option<std::string> modulesDir;
  std::string httpAuthenticatee;
};

} // namespace scheduler {
} // namespace v1 {
} // namespace mesos {

#endif // __SCHEDULER_FLAGS_HPP__
