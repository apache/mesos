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

#ifndef __MASTER_FLAGS_HPP__
#define __MASTER_FLAGS_HPP__

#include <cstdint>

#include <string>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>

#include <mesos/mesos.hpp>

#include <mesos/authorizer/acls.hpp>

#include <mesos/module/module.hpp>

#include "logging/flags.hpp"

#include "messages/flags.hpp"

namespace mesos {
namespace internal {
namespace master {

class Flags : public virtual logging::Flags
{
public:
  Flags();
  bool version;
  Option<std::string> hostname;
  bool hostname_lookup;
  bool root_submissions;
  Option<std::string> work_dir;
  std::string registry;
  Option<int> quorum;
  Duration zk_session_timeout;
  bool registry_strict;
  Duration registry_fetch_timeout;
  Duration registry_store_timeout;
  bool log_auto_initialize;
  Duration agent_reregister_timeout;
  std::string recovery_agent_removal_limit;
  Option<std::string> agent_removal_rate_limit;
  std::string webui_dir;
  Option<Path> whitelist;
  std::string role_sorter;
  std::string framework_sorter;
  Duration allocation_interval;
  Option<std::string> cluster;
  Option<std::string> roles;
  Option<std::string> weights;
  bool authenticate_frameworks;
  bool authenticate_agents;
  Duration authentication_v0_timeout;
  bool authenticate_http_readonly;
  bool authenticate_http_readwrite;
  bool authenticate_http_frameworks;
  Option<Path> credentials;
  Option<ACLs> acls;
  Option<Firewall> firewall_rules;
  Option<RateLimits> rate_limits;
  Option<Duration> offer_timeout;
  Option<Modules> modules;
  Option<std::string> modulesDir;
  std::string authenticators;
  std::string allocator;
  double allocator_agent_recovery_factor;
  Duration allocator_recovery_timeout;
  Option<std::set<std::string>> fair_sharing_excluded_resource_names;
  bool filter_gpu_resources;
  std::string min_allocatable_resources;
  Option<std::string> hooks;
  Duration agent_ping_timeout;
  size_t max_agent_ping_timeouts;
  std::string authorizers;
  std::string http_authenticators;
  Option<std::string> http_framework_authenticators;
  size_t max_operator_event_stream_subscribers;
  size_t max_completed_frameworks;
  size_t max_completed_tasks_per_framework;
  size_t max_unreachable_tasks_per_framework;
  Option<std::string> master_contender;
  Option<std::string> master_detector;
  Duration registry_gc_interval;
  Duration registry_max_agent_age;
  size_t registry_max_agent_count;
  bool require_agent_domain;
  bool publish_per_framework_metrics;
  Option<DomainInfo> domain;

  // The following flags are executable specific (e.g., since we only
  // have one instance of libprocess per execution, we only want to
  // advertise the IP and port option once, here).

  Option<std::string> ip;
  uint16_t port;
  Option<std::string> advertise_ip;
  Option<std::string> advertise_port;
  Option<flags::SecurePathOrValue> zk;
  bool memory_profiling;

  // Optional IP discover script that will set the Master IP.
  // If set, its output is expected to be a valid parseable IP string.
  Option<std::string> ip_discovery_command;

#ifdef ENABLE_PORT_MAPPING_ISOLATOR
  Option<size_t> max_executors_per_agent;
#endif  // ENABLE_PORT_MAPPING_ISOLATOR

  Bytes offer_constraints_re2_max_mem;
  int offer_constraints_re2_max_program_size;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_FLAGS_HPP__
