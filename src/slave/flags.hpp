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
  Option<std::string> resource_provider_config_dir;
  Option<std::string> csi_plugin_config_dir;
  Option<std::string> disk_profile_adaptor;
  std::string isolation;
  std::string launcher;

  Option<std::string> image_providers;
  Option<std::string> image_provisioner_backend;
  Option<ImageGcConfig> image_gc_config;

  std::string appc_simple_discovery_uri_prefix;
  std::string appc_store_dir;

  std::string docker_registry;
  std::string docker_store_dir;
  std::string docker_volume_checkpoint_dir;
  bool docker_volume_chown;
  bool docker_ignore_runtime;

  std::string default_role;
  Option<std::string> attributes;
  Bytes fetcher_cache_size;
  std::string fetcher_cache_dir;
  Duration fetcher_stall_timeout;
  std::string work_dir;
  std::string runtime_dir;
  std::string launcher_dir;
  Option<std::string> hadoop_home;
  size_t max_completed_executors_per_framework;

#ifndef __WINDOWS__
  bool switch_user;
  Option<std::string> volume_gid_range;
#endif // __WINDOWS__
  Duration http_heartbeat_interval;
  std::string frameworks_home;  // TODO(benh): Make an Option.
  Duration registration_backoff_factor;
  Duration authentication_backoff_factor;
  Duration authentication_timeout_min;
  Duration authentication_timeout_max;
  Option<JSON::Object> executor_environment_variables;
  Duration executor_registration_timeout;
  Duration executor_reregistration_timeout;
  Option<Duration> executor_reregistration_retry_interval;
  Duration executor_shutdown_grace_period;
#ifdef USE_SSL_SOCKET
  Option<Path> jwt_secret_key;
#endif // USE_SSL_SOCKET
  Duration gc_delay;
  double gc_disk_headroom;
  bool gc_non_executor_container_sandboxes;
  Duration disk_watch_interval;

  Option<std::string> container_logger;

  std::string reconfiguration_policy;
  std::string recover;
  Duration recovery_timeout;
  bool strict;
  Duration register_retry_interval_min;
#ifdef __linux__
  Duration cgroups_destroy_timeout;
  std::string cgroups_hierarchy;
  std::string cgroups_root;
  bool cgroups_enable_cfs;
  bool cgroups_limit_swap;
  bool cgroups_cpu_enable_pids_and_tids_count;
  Option<std::string> cgroups_net_cls_primary_handle;
  Option<std::string> cgroups_net_cls_secondary_handles;
  Option<DeviceWhitelist> allowed_devices;
  Option<std::string> agent_subsystems;
  Option<std::string> host_path_volume_force_creation;
  Option<std::vector<unsigned int>> nvidia_gpu_devices;
  Option<std::string> perf_events;
  Duration perf_interval;
  Duration perf_duration;
  bool revocable_cpu_low_priority;
  bool systemd_enable_support;
  std::string systemd_runtime_directory;
  Option<CapabilityInfo> effective_capabilities;
  Option<CapabilityInfo> bounding_capabilities;
  Option<Bytes> default_container_shm_size;
  bool disallow_sharing_agent_ipc_namespace;
  bool disallow_sharing_agent_pid_namespace;
#endif
  Option<Firewall> firewall_rules;
  Option<Path> credential;
  Option<ACLs> acls;
  std::string containerizers;
  std::string docker;
  Option<std::string> docker_mesos_image;
  Duration docker_remove_delay;
  std::string sandbox_directory;
  Option<ContainerDNSInfo> default_container_dns;
  Option<ContainerInfo> default_container_info;

  // TODO(alexr): Remove this after the deprecation cycle (started in 1.0).
  Duration docker_stop_timeout;

  bool docker_kill_orphans;
  std::string docker_socket;
  Option<JSON::Object> docker_config;

#ifdef ENABLE_PORT_MAPPING_ISOLATOR
  uint16_t ephemeral_ports_per_container;
  Option<std::string> eth0_name;
  Option<std::string> lo_name;
  Option<Bytes> egress_rate_limit_per_container;
  bool egress_unique_flow_per_container;
  std::string egress_flow_classifier_parent;
  bool network_enable_socket_statistics_summary;
  bool network_enable_socket_statistics_details;
  bool network_enable_snmp_statistics;
#endif // ENABLE_PORT_MAPPING_ISOLATOR

#ifdef ENABLE_NETWORK_PORTS_ISOLATOR
  Duration container_ports_watch_interval;
  bool check_agent_port_range_only;
  bool enforce_container_ports;
  Option<std::string> container_ports_isolated_range;
#endif // ENABLE_NETWORK_PORTS_ISOLATOR

  Option<std::string> network_cni_plugins_dir;
  Option<std::string> network_cni_config_dir;
  bool network_cni_root_dir_persist;
  bool network_cni_metrics;
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
#ifndef __WINDOWS__
  bool http_executor_domain_sockets;
  Option<std::string> domain_socket_location;
#endif // __WINDOWS__
  Option<Path> http_credentials;
  Option<std::string> hooks;
  Option<std::string> secret_resolver;
  Option<std::string> resource_estimator;
  Option<std::string> qos_controller;
  Duration qos_correction_interval_min;
  Duration oversubscribed_resources_interval;
  Option<std::string> master_detector;
#if ENABLE_XFS_DISK_ISOLATOR
  std::string xfs_project_range;
  bool xfs_kill_containers;
#endif
#if ENABLE_SECCOMP_ISOLATOR
  Option<std::string> seccomp_config_dir;
  Option<std::string> seccomp_profile_name;
#endif
  bool http_command_executor;
  Option<SlaveCapabilities> agent_features;
  Option<DomainInfo> domain;

  // The following flags are executable specific (e.g., since we only
  // have one instance of libprocess per execution, we only want to
  // advertise the IP and port option once, here).

  Option<std::string> ip;
  uint16_t port;
  Option<std::string> advertise_ip;
  Option<std::string> advertise_port;
  Option<flags::SecurePathOrValue> master;
  bool memory_profiling;

  Duration zk_session_timeout;

  // Optional IP discover script that will set the slave's IP.
  // If set, its output is expected to be a valid parseable IP string.
  Option<std::string> ip_discovery_command;

  // IPv6 flags.
  //
  // NOTE: These IPv6 flags are currently input mechanisms
  // for the operator to specify v6 addresses on which containers
  // running on host network can listen. Mesos itself doesn't listen
  // or communicate over v6 addresses at this point.
  Option<std::string> ip6;

  // Similar to the `ip_discovery_command` this optional discover
  // script is expected to output a valid IPv6 string. Only one of the
  // two options `ip6` or `ip6_discovery_command` can be set at any
  // given point of time.
  Option<std::string> ip6_discovery_command;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_FLAGS_HPP__
