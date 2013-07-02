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

#ifndef __MASTER_FLAGS_HPP__
#define __MASTER_FLAGS_HPP__

#include <string>

#include <stout/duration.hpp>
#include <stout/flags.hpp>

#include "logging/flags.hpp"

namespace mesos {
namespace internal {
namespace master {

class Flags : public logging::Flags
{
public:
  Flags()
  {
    add(&Flags::root_submissions,
        "root_submissions",
        "Can root submit frameworks?",
        true);

    add(&Flags::slaves,
        "slaves",
        "Initial slaves that should be\n"
        "considered part of this cluster\n"
        "(or if using ZooKeeper a URL)",
        "*");

    add(&Flags::webui_dir,
        "webui_dir",
        "Location of the webui files/assets",
        MESOS_WEBUI_DIR);

    add(&Flags::whitelist,
        "whitelist",
        "Path to a file with a list of slaves\n"
        "(one per line) to advertise offers for;\n"
        "should be of the form: file://path/to/file",
        "*");

    add(&Flags::user_sorter,
        "user_sorter",
        "Policy to use for allocating resources\n"
        "between users. May be one of:\n"
        "  dominant_resource_fairness (drf)",
        "drf");
 
    add(&Flags::framework_sorter,
        "framework_sorter",
        "Policy to use for allocating resources\n"
        "between a given user's frameworks. Options\n"
        "are the same as for user_allocator",
        "drf");

    add(&Flags::allocation_interval,
        "allocation_interval",
        "Amount of time to wait between performing\n"
        " (batch) allocations (e.g., 500ms, 1sec, etc)",
        Seconds(1));

    add(&Flags::cluster,
        "cluster",
        "Human readable name for the cluster,\n"
        "displayed in the webui");

    add(&Flags::roles,
        "roles",
        "A comma seperated list of the allocation\n"
        "roles that frameworks in this cluster may\n"
        "belong to.");

    add(&Flags::weights,
        "weights",
        "A comma seperated list of role/weight pairs\n"
        "of the form 'role=weight,role=weight'. Weights\n"
        "are used to indicate forms of priority.");
  }

  bool root_submissions;
  std::string slaves;
  std::string webui_dir;
  std::string whitelist;
  std::string user_sorter;
  std::string framework_sorter;
  Duration allocation_interval;
  Option<std::string> cluster;
  Option<std::string> roles;
  Option<std::string> weights;
};

} // namespace mesos {
} // namespace internal {
} // namespace master {

#endif // __MASTER_FLAGS_HPP__
