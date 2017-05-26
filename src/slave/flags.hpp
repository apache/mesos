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

#include <cstdint>
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

class Flags : public virtual logging::Flags
{
public:
  Flags();

  bool version;
  Option<std::string> hostname;
  bool hostname_lookup;
  Option<std::string> resources;
  std::string isolation;
  std::string launcher;

  Option<std::string> image_providers;
  Option<std::string> image_provisioner_backend;

  std::string appc_simple_discovery_uri_prefix;
  std::string appc_store_dir;

  std::string docker_registry;
  std::string docker_store_dir;
  std::string docker_volume_checkpoint_dir;

  std::string default_role;
  Option<std::string> attributes;
  Bytes fetcher_cache_size;
  std::string fetcher_cache_dir;
  std::string work_dir;
  std::string runtime_dir;
  std::string launcher_dir;
  std::string hadoop_home; // TODO(benh): Make an Option.
  size_t max_completed_executors_per_framework;

#ifndef __WINDOWS__
  bool switch_user;
#endif // __WINDOWS__
  Duration http_heartbeat_interval;
  std::string frameworks_home;  // TODO(benh): Make an Option.
  Duration registration_backoff_factor;
  Duration authentication_backoff_factor;
  Option<JSON::Object> executor_environment_variables;
  Duration executor_registration_timeout;
  Duration executor_reregistration_timeout;
  Option<Duration> executor_reregistration_retry_interval;
  Duration executor_shutdown_grace_period;
#ifdef USE_SSL_SOCKET
  Option<Path> executor_secret_key;
#endif // USE_SSL_SOCKET
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
  Option<DeviceWhitelist> allowed_devices;
  Option<std::string> agent_subsystems;
  Option<std::vector<unsigned int>> nvidia_gpu_devices;
  Option<std::string> perf_events;
  Duration perf_interval;
  Duration perf_duration;
  bool revocable_cpu_low_priority;
  bool systemd_enable_support;
  std::string systemd_runtime_directory;
  Option<CapabilityInfo> allowed_capabilities;
#endif
  Option<Firewall> firewall_rules;
  Option<Path> credential;
  Option<ACLs> acls;
  std::string containerizers;
  std::string docker;
  Option<std::string> docker_mesos_image;
  Duration docker_remove_delay;
  std::string sandbox_directory;
  Option<ContainerInfo> default_container_info;

  // TODO(alexr): Remove this after the deprecation cycle (started in 1.0).
  Duration docker_stop_timeout;

  bool docker_kill_orphans;
  std::string docker_socket;
  Option<JSON::Object> docker_config;

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
  Option<std::string> modulesDir;
  std::string authenticatee;
  std::string authorizer;
  Option<std::string> http_authenticators;
  bool authenticate_http_readonly;
  bool authenticate_http_readwrite;
#ifdef USE_SSL_SOCKET
  bool authenticate_http_executors;
#endif // USE_SSL_SOCKET
  Option<Path> http_credentials;
  Option<std::string> hooks;
  Option<std::string> resource_estimator;
  Option<std::string> qos_controller;
  Duration qos_correction_interval_min;
  Duration oversubscribed_resources_interval;
  Option<std::string> master_detector;
#if ENABLE_XFS_DISK_ISOLATOR
  std::string xfs_project_range;
#endif
  bool http_command_executor;

  // The following flags are executable specific (e.g., since we only
  // have one instance of libprocess per execution, we only want to
  // advertise the IP and port option once, here).

  Option<std::string> ip;
  uint16_t port;
  Option<std::string> advertise_ip;
  Option<std::string> advertise_port;
  Option<std::string> master;

  // Optional IP discover script that will set the slave's IP.
  // If set, its output is expected to be a valid parseable IP string.
  Option<std::string> ip_discovery_command;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_FLAGS_HPP__
