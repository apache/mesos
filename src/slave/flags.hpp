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

#ifndef __SLAVE_FLAGS_HPP__
#define __SLAVE_FLAGS_HPP__

#include <string>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>

#include <mesos/module/module.hpp>

#include "logging/flags.hpp"

#include "messages/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

class Flags : public logging::Flags
{
public:
  Flags();

  bool version;
  Option<std::string> hostname;
  bool hostname_lookup;
  Option<std::string> resources;
  std::string isolation;
  Option<std::string> launcher;

  Option<std::string> image_providers;
  std::string image_provisioner_backend;

  std::string appc_simple_discovery_uri_prefix;
  std::string appc_store_dir;

  std::string docker_registry;
  std::string docker_store_dir;

  std::string default_role;
  Option<std::string> attributes;
  Bytes fetcher_cache_size;
  std::string fetcher_cache_dir;
  std::string work_dir;
  std::string launcher_dir;
  std::string hadoop_home; // TODO(benh): Make an Option.
#ifndef __WINDOWS__
  bool switch_user;
#endif // __WINDOWS__
  std::string frameworks_home;  // TODO(benh): Make an Option.
  Duration registration_backoff_factor;
  Option<JSON::Object> executor_environment_variables;
  Duration executor_registration_timeout;
  Duration executor_shutdown_grace_period;
  Duration gc_delay;
  double gc_disk_headroom;
  Duration disk_watch_interval;

  Option<std::string> container_logger;

  std::string recover;
  Duration recovery_timeout;
  bool strict;
  Duration register_retry_interval_min;
#ifdef __linux__
  std::string cgroups_hierarchy;
  std::string cgroups_root;
  bool cgroups_enable_cfs;
  bool cgroups_limit_swap;
  bool cgroups_cpu_enable_pids_and_tids_count;
  Option<std::string> cgroups_net_cls_primary_handle;
  Option<std::string> cgroups_net_cls_secondary_handles;
  Option<std::string> slave_subsystems;
  Option<std::string> perf_events;
  Duration perf_interval;
  Duration perf_duration;
  bool revocable_cpu_low_priority;
  bool systemd_enable_support;
  std::string systemd_runtime_directory;
#endif
  Option<Firewall> firewall_rules;
  Option<Path> credential;
  Option<std::string> containerizer_path;
  std::string containerizers;
  Option<std::string> default_container_image;
  std::string docker;
  Option<std::string> docker_mesos_image;
  Duration docker_remove_delay;
  std::string sandbox_directory;
  Option<ContainerInfo> default_container_info;
  Duration docker_stop_timeout;
  bool docker_kill_orphans;
  std::string docker_socket;
#ifdef ENABLE_NVIDIA_GPU_SUPPORT
  Option<std::vector<unsigned int>> nvidia_gpu_devices;
#endif
#ifdef WITH_NETWORK_ISOLATOR
  uint16_t ephemeral_ports_per_container;
  Option<std::string> eth0_name;
  Option<std::string> lo_name;
  Option<Bytes> egress_rate_limit_per_container;
  bool egress_unique_flow_per_container;
  std::string egress_flow_classifier_parent;
  bool network_enable_socket_statistics_summary;
  bool network_enable_socket_statistics_details;
  bool network_enable_snmp_statistics;
#endif
  Option<std::string> network_cni_plugins_dir;
  Option<std::string> network_cni_config_dir;
  Duration container_disk_watch_interval;
  bool enforce_container_disk_quota;
  Option<Modules> modules;
  std::string authenticatee;
  std::string http_authenticators;
  bool authenticate_http;
  Option<Path> http_credentials;
  Option<std::string> hooks;
  Option<std::string> resource_estimator;
  Option<std::string> qos_controller;
  Duration qos_correction_interval_min;
  Duration oversubscribed_resources_interval;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_FLAGS_HPP__
