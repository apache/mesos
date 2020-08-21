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

#include <stdint.h>

#include <set>
#include <vector>
#include <utility>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/master/detector.hpp>

#include <mesos/mesos.hpp>

#include <mesos/module/anonymous.hpp>
#include <mesos/module/secret_resolver.hpp>

#include <mesos/secret/resolver.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include <process/network.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <process/ssl/flags.hpp>

#include <stout/check.hpp>
#include <stout/flags.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#include <stout/os/permissions.hpp>

#ifdef __linux__
#include <linux/systemd.hpp>

#include <stout/proc.hpp>
#endif // __linux__

#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/version.hpp>

#ifdef USE_SSL_SOCKET
#include "authentication/executor/jwt_secret_generator.hpp"
#endif // USE_SSL_SOCKET

#include "common/authorization.hpp"
#include "common/build.hpp"

#ifndef __WINDOWS__
#include "common/domain_sockets.hpp"
#endif // __WINDOWS__

#include "common/http.hpp"

#include "hook/manager.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#include "linux/systemd.hpp"
#endif // __linux__

#include "logging/logging.hpp"

#include "messages/flags.hpp"
#include "messages/messages.hpp"

#include "module/manager.hpp"

#include "slave/constants.hpp"
#include "slave/csi_server.hpp"
#include "slave/gc.hpp"
#include "slave/slave.hpp"
#include "slave/task_status_update_manager.hpp"

#include "version/version.hpp"

using namespace mesos::internal;
using namespace mesos::internal::slave;

using mesos::SecretGenerator;

#ifdef USE_SSL_SOCKET
using mesos::authentication::executor::JWTSecretGenerator;
#endif // USE_SSL_SOCKET

using mesos::master::detector::MasterDetector;

using mesos::modules::Anonymous;
using mesos::modules::ModuleManager;

using mesos::slave::QoSController;
using mesos::slave::ResourceEstimator;

using mesos::Authorizer;
using mesos::SecretResolver;
using mesos::SlaveInfo;

using net::IP;

using process::Owned;

using process::firewall::DisabledEndpointsFirewallRule;
using process::firewall::FirewallRule;

#ifndef __WINDOWS__
using process::network::unix::Socket;
#endif // __WINDOWS__

using std::cerr;
using std::cout;
using std::endl;
using std::move;
using std::set;
using std::string;
using std::vector;


#ifdef __linux__
// Move the slave into its own cgroup for each of the specified
// subsystems.
//
// NOTE: Any subsystem configuration is inherited from the mesos
// root cgroup for that subsystem, e.g., by default the memory
// cgroup will be unlimited.
//
// TODO(jieyu): Make sure the corresponding cgroup isolator is
// enabled so that the container processes are moved to different
// cgroups than the agent cgroup.
static Try<Nothing> assignCgroups(const slave::Flags& flags)
{
  CHECK_SOME(flags.agent_subsystems);

  foreach (const string& subsystem,
           strings::tokenize(flags.agent_subsystems.get(), ",")) {
    LOG(INFO) << "Moving agent process into its own cgroup for"
              << " subsystem: " << subsystem;

    // Ensure the subsystem is mounted and the Mesos root cgroup is
    // present.
    Try<string> hierarchy = cgroups::prepare(
        flags.cgroups_hierarchy,
        subsystem,
        flags.cgroups_root);

    if (hierarchy.isError()) {
      return Error(
          "Failed to prepare cgroup " + flags.cgroups_root +
          " for subsystem " + subsystem +
          ": " + hierarchy.error());
    }

    // Create a cgroup for the slave.
    string cgroup = path::join(flags.cgroups_root, "slave");

    if (!cgroups::exists(hierarchy.get(), cgroup)) {
      Try<Nothing> create = cgroups::create(hierarchy.get(), cgroup);
      if (create.isError()) {
        return Error(
            "Failed to create cgroup " + cgroup +
            " for subsystem " + subsystem +
            " under hierarchy " + hierarchy.get() +
            " for agent: " + create.error());
      }
    }

    // Exit if there are processes running inside the cgroup - this
    // indicates a prior slave (or child process) is still running.
    Try<set<pid_t>> processes = cgroups::processes(hierarchy.get(), cgroup);
    if (processes.isError()) {
      return Error(
          "Failed to check for existing threads in cgroup " + cgroup +
          " for subsystem " + subsystem +
          " under hierarchy " + hierarchy.get() +
          " for agent: " + processes.error());
    }

    // Log if there are any processes in the slave's cgroup. They
    // may be transient helper processes like 'perf' or 'du',
    // ancillary processes like 'docker log' or possibly a stuck
    // slave.
    // TODO(idownes): Generally, it's not a problem if there are
    // processes running in the slave's cgroup, though any resources
    // consumed by those processes are accounted to the slave. Where
    // applicable, transient processes should be configured to
    // terminate if the slave exits; see example usage for perf in
    // isolators/cgroups/perf.cpp. Consider moving ancillary
    // processes to a different cgroup, e.g., moving 'docker log' to
    // the container's cgroup.
    if (!processes->empty()) {
      // For each process, we print its pid as well as its command
      // to help triaging.
      vector<string> infos;
      foreach (pid_t pid, processes.get()) {
        Result<os::Process> proc = os::process(pid);

        // Only print the command if available.
        if (proc.isSome()) {
          infos.push_back(stringify(pid) + " '" + proc->command + "'");
        } else {
          infos.push_back(stringify(pid));
        }
      }

      LOG(INFO) << "An agent (or child process) is still running, please"
                << " consider checking the following process(es) listed in "
                << path::join(hierarchy.get(), cgroup, "cgroup.procs")
                << ":\n" << strings::join("\n", infos);
    }

    // Move all of our threads into the cgroup.
    Try<Nothing> assign = cgroups::assign(hierarchy.get(), cgroup, getpid());
    if (assign.isError()) {
      return Error(
          "Failed to move agent into cgroup " + cgroup +
          " for subsystem " + subsystem +
          " under hierarchy " + hierarchy.get() +
          " for agent: " + assign.error());
    }
  }

  return Nothing();
}
#endif // __linux__


int main(int argc, char** argv)
{
  // The order of initialization is as follows:
  // * Windows socket stack.
  // * Validate flags.
  // * Logging
  // * Log build information.
  // * Libprocess
  // * Version process
  // * Firewall rules: should be initialized before initializing HTTP endpoints.
  // * Modules: Load module libraries and manifests before they
  //   can be instantiated.
  // * Anonymous modules: Later components such as Allocators, and master
  //   contender/detector might depend upon anonymous modules.
  // * Hooks.
  // * Systemd support (if it exists).
  // * Fetcher, SecretResolver, and Containerizer.
  // * Master detector.
  // * Authorizer.
  // * Garbage collector.
  // * Task status update manager.
  // * Resource estimator.
  // * QoS controller.
  // * `Agent` process.
  //
  // TODO(avinash): Add more comments discussing the rationale behind for this
  // particular component ordering.

  GOOGLE_PROTOBUF_VERIFY_VERSION;

  slave::Flags flags;

  Try<flags::Warnings> load = flags.load("MESOS_", argc, argv);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (flags.version) {
    cout << "mesos" << " " << MESOS_VERSION << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << load.error() << "\n\n"
         << "See `mesos-agent --help` for a list of supported flags." << endl;
    return EXIT_FAILURE;
  }

  logging::initialize(argv[0], true, flags); // Catch signals.

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  // Check that agent's version has the expected format (SemVer).
  {
    Try<Version> version = Version::parse(MESOS_VERSION);
    if (version.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to parse Mesos version '" << MESOS_VERSION << "': "
        << version.error();
    }
  }

  if (flags.master.isNone() && flags.master_detector.isNone()) {
    EXIT(EXIT_FAILURE)
      << "Missing required option `--master` or `--master_detector`";
  }

  if (flags.master.isSome() && flags.master_detector.isSome()) {
    EXIT(EXIT_FAILURE)
      << "Only one of `--master` or `--master_detector` should be specified";
  }

  // Initialize libprocess.
  if (flags.ip_discovery_command.isSome() && flags.ip.isSome()) {
    EXIT(EXIT_FAILURE)
      << "Only one of `--ip` or `--ip_discovery_command` should be specified";
  }

  if (flags.ip6_discovery_command.isSome() && flags.ip6.isSome()) {
    EXIT(EXIT_FAILURE)
      << "Only one of `--ip6` or `--ip6_discovery_command` should be specified";
  }

  if (flags.ip_discovery_command.isSome()) {
    Try<string> ipAddress = os::shell(flags.ip_discovery_command.get());

    if (ipAddress.isError()) {
      EXIT(EXIT_FAILURE) << ipAddress.error();
    }

    os::setenv("LIBPROCESS_IP", strings::trim(ipAddress.get()));
  } else if (flags.ip.isSome()) {
    os::setenv("LIBPROCESS_IP", flags.ip.get());
  }

  if (flags.ip6_discovery_command.isSome()) {
    Try<string> ip6Address = os::shell(flags.ip6_discovery_command.get());
    if (ip6Address.isError()) {
      EXIT(EXIT_FAILURE) << ip6Address.error();
    }

    os::setenv("LIBPROCESS_IP6", strings::trim(ip6Address.get()));
  } else if (flags.ip6.isSome()) {
    os::setenv("LIBPROCESS_IP6", flags.ip6.get());
  }

  os::setenv("LIBPROCESS_PORT", stringify(flags.port));

  if (flags.advertise_ip.isSome()) {
    os::setenv("LIBPROCESS_ADVERTISE_IP", flags.advertise_ip.get());
  }

  if (flags.advertise_port.isSome()) {
    os::setenv("LIBPROCESS_ADVERTISE_PORT", flags.advertise_port.get());
  }

#ifndef __WINDOWS__
  if (flags.http_executor_domain_sockets) {
    if (flags.domain_socket_location.isNone()) {
      flags.domain_socket_location =
        flags.runtime_dir + "/" + AGENT_EXECUTORS_SOCKET_FILENAME;
    }

    if (flags.domain_socket_location->size() >=
        common::DOMAIN_SOCKET_MAX_PATH_LENGTH) {
      EXIT(EXIT_FAILURE)
        << "Domain socket location '" << *flags.domain_socket_location << "'"
        << " must have less than 108 characters.";
    }
  }
#endif // __WINDOWS__

  os::setenv("LIBPROCESS_MEMORY_PROFILING", stringify(flags.memory_profiling));

  // Log build information.
  LOG(INFO) << "Build: " << build::DATE << " by " << build::USER;
  LOG(INFO) << "Version: " << MESOS_VERSION;

  if (build::GIT_TAG.isSome()) {
    LOG(INFO) << "Git tag: " << build::GIT_TAG.get();
  }

  if (build::GIT_SHA.isSome()) {
    LOG(INFO) << "Git SHA: " << build::GIT_SHA.get();
  }

#ifdef __linux__
  // Move the agent process into its own cgroup for each of the specified
  // subsystems if necessary before the process is initialized.
  if (flags.agent_subsystems.isSome()) {
    Try<Nothing> assign = assignCgroups(flags);
    if (assign.isError()) {
      EXIT(EXIT_FAILURE) << assign.error();
    }
  }
#endif // __linux__

  const string id = process::ID::generate("slave"); // Process ID.

  // If `process::initialize()` returns `false`, then it was called before this
  // invocation, meaning the authentication realm for libprocess-level HTTP
  // endpoints was set incorrectly. This should be the first invocation.
  if (!process::initialize(
          id,
          READWRITE_HTTP_AUTHENTICATION_REALM,
          READONLY_HTTP_AUTHENTICATION_REALM)) {
    EXIT(EXIT_FAILURE) << "The call to `process::initialize()` in the agent's "
                       << "`main()` was not the function's first invocation";
  }

  spawn(new VersionProcess(), true);

  if (flags.firewall_rules.isSome()) {
    vector<Owned<FirewallRule>> rules;

    const Firewall firewall = flags.firewall_rules.get();

    if (firewall.has_disabled_endpoints()) {
      hashset<string> paths;

      foreach (const string& path, firewall.disabled_endpoints().paths()) {
        paths.insert(path);
      }

      rules.emplace_back(new DisabledEndpointsFirewallRule(paths));
    }

    process::firewall::install(move(rules));
  }

  // Initialize modules.
  if (flags.modules.isSome() && flags.modulesDir.isSome()) {
    EXIT(EXIT_FAILURE) <<
      "Only one of --modules or --modules_dir should be specified";
  }

  if (flags.modulesDir.isSome()) {
    Try<Nothing> result = ModuleManager::load(flags.modulesDir.get());
    if (result.isError()) {
      EXIT(EXIT_FAILURE) << "Error loading modules: " << result.error();
    }
  }

  if (flags.modules.isSome()) {
    Try<Nothing> result = ModuleManager::load(flags.modules.get());
    if (result.isError()) {
      EXIT(EXIT_FAILURE) << "Error loading modules: " << result.error();
    }
  }

  // Create anonymous modules.
  foreach (const string& name, ModuleManager::find<Anonymous>()) {
    Try<Anonymous*> create = ModuleManager::create<Anonymous>(name);
    if (create.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create anonymous module named '" << name << "'";
    }

    // We don't bother keeping around the pointer to this anonymous
    // module, when we exit that will effectively free its memory.
    //
    // TODO(benh): We might want to add explicit finalization (and
    // maybe explicit initialization too) in order to let the module
    // do any housekeeping necessary when the slave is cleanly
    // terminating.
  }

  // Initialize hooks.
  if (flags.hooks.isSome()) {
    Try<Nothing> result = HookManager::initialize(flags.hooks.get());
    if (result.isError()) {
      EXIT(EXIT_FAILURE) << "Error installing hooks: " << result.error();
    }
  }

#ifdef __linux__
  // Initialize systemd if it exists.
  if (flags.systemd_enable_support && systemd::exists()) {
    LOG(INFO) << "Initializing systemd state";

    systemd::Flags systemdFlags;
    systemdFlags.enabled = flags.systemd_enable_support;
    systemdFlags.runtime_directory = flags.systemd_runtime_directory;
    systemdFlags.cgroups_hierarchy = flags.cgroups_hierarchy;

    Try<Nothing> initialize = systemd::initialize(systemdFlags);
    if (initialize.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to initialize systemd: " + initialize.error();
    }
  }
#endif // __linux__

  Fetcher* fetcher = new Fetcher(flags);
  GarbageCollector* gc = new GarbageCollector(flags.work_dir);

  // Initialize SecretResolver.
  Try<SecretResolver*> secretResolver =
    mesos::SecretResolver::create(flags.secret_resolver);

  if (secretResolver.isError()) {
    EXIT(EXIT_FAILURE)
        << "Failed to initialize secret resolver: " << secretResolver.error();
  }

  VolumeGidManager* volumeGidManager = nullptr;

#ifndef __WINDOWS__
  if (flags.volume_gid_range.isSome()) {
    Try<VolumeGidManager*> _volumeGidManager = VolumeGidManager::create(flags);
    if (_volumeGidManager.isError()) {
      EXIT(EXIT_FAILURE) << "Failed to initialize volume gid manager: "
                         << _volumeGidManager.error();
    }

    volumeGidManager = _volumeGidManager.get();
  }
#endif // __WINDOWS__

  // Initialize PendingFutureTracker.
  Try<PendingFutureTracker*> futureTracker = PendingFutureTracker::create();
  if (futureTracker.isError()) {
    EXIT(EXIT_FAILURE) << "Failed to initialize pending future tracker: "
                       << futureTracker.error();
  }

  SecretGenerator* secretGenerator = nullptr;

#ifdef USE_SSL_SOCKET
  if (flags.jwt_secret_key.isSome()) {
    Try<string> jwtSecretKey = os::read(flags.jwt_secret_key.get());
    if (jwtSecretKey.isError()) {
      EXIT(EXIT_FAILURE) << "Failed to read the file specified by "
                         << "--jwt_secret_key";
    }

    // TODO(greggomann): Factor the following code out into a common helper,
    // since we also do this when loading credentials.
    Try<os::Permissions> permissions =
      os::permissions(flags.jwt_secret_key.get());
    if (permissions.isError()) {
      LOG(WARNING) << "Failed to stat jwt secret key file '"
                   << flags.jwt_secret_key.get()
                   << "': " << permissions.error();
    } else if (permissions->others.rwx) {
      LOG(WARNING) << "Permissions on executor secret key file '"
                   << flags.jwt_secret_key.get()
                   << "' are too open; it is recommended that your"
                   << " key file is NOT accessible by others";
    }

    secretGenerator = new JWTSecretGenerator(jwtSecretKey.get());
  }
#endif // USE_SSL_SOCKET

  // The agent will hold ownership of the CSI server, but we also pass a pointer
  // to it into the containerizer for use by the 'volume/csi' isolator.
  Owned<CSIServer> csiServer;

  if (flags.csi_plugin_config_dir.isSome()) {
    // Initialize the CSI server, which manages any configured CSI plugins.
    string scheme = "http";

#ifdef USE_SSL_SOCKET
    if (process::network::openssl::flags().enabled) {
      scheme = "https";
    }
#endif

    const process::http::URL agentUrl(
        scheme,
        process::address().ip,
        process::address().port,
        id + "/api/v1");

    Try<Owned<CSIServer>> csiServer_ = CSIServer::create(
        flags,
        agentUrl,
        secretGenerator,
        secretResolver.get());

    if (csiServer_.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to initialize the CSI server: " << csiServer_.error();
    }

    csiServer = std::move(csiServer_.get());
  }

  Try<Containerizer*> containerizer = Containerizer::create(
      flags,
      false,
      fetcher,
      gc,
      secretResolver.get(),
      volumeGidManager,
      futureTracker.get(),
      csiServer.get());

  if (containerizer.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create a containerizer: " << containerizer.error();
  }

  Try<MasterDetector*> detector_ = MasterDetector::create(
      flags.master.isSome() ? flags.master->value : Option<string>::none(),
      flags.master_detector,
      flags.zk_session_timeout);

  if (detector_.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create a master detector: " << detector_.error();
  }

  MasterDetector* detector = detector_.get();

  Option<Authorizer*> authorizer_ = None();

  string authorizerName = flags.authorizer;

  Result<Authorizer*> authorizer((None()));
  if (authorizerName != slave::DEFAULT_AUTHORIZER) {
    LOG(INFO) << "Creating '" << authorizerName << "' authorizer";

    // NOTE: The contents of --acls will be ignored.
    authorizer = Authorizer::create(authorizerName);
  } else {
    // `authorizerName` is `DEFAULT_AUTHORIZER` at this point.
    if (flags.acls.isSome()) {
      LOG(INFO) << "Creating default '" << authorizerName << "' authorizer";

      authorizer = Authorizer::create(flags.acls.get());
    }
  }

  if (authorizer.isError()) {
    EXIT(EXIT_FAILURE) << "Could not create '" << authorizerName
                       << "' authorizer: " << authorizer.error();
  } else if (authorizer.isSome()) {
    authorizer_ = authorizer.get();

    // Set the authorization callbacks for libprocess HTTP endpoints.
    // Note that these callbacks capture `authorizer_.get()`, but the agent
    // creates a copy of the authorizer during construction. Thus, if in the
    // future it becomes possible to dynamically set the authorizer, this would
    // break.
    process::http::authorization::setCallbacks(
        mesos::authorization::createAuthorizationCallbacks(authorizer_.get()));
  }

  Files* files = new Files(READONLY_HTTP_AUTHENTICATION_REALM, authorizer_);
  TaskStatusUpdateManager* taskStatusUpdateManager =
    new TaskStatusUpdateManager(flags);

  Try<ResourceEstimator*> resourceEstimator =
    ResourceEstimator::create(flags.resource_estimator);

  if (resourceEstimator.isError()) {
    EXIT(EXIT_FAILURE) << "Failed to create resource estimator: "
                       << resourceEstimator.error();
  }

  Try<QoSController*> qosController =
    QoSController::create(flags.qos_controller);

  if (qosController.isError()) {
    EXIT(EXIT_FAILURE) << "Failed to create QoS Controller: "
                       << qosController.error();
  }

#ifndef __WINDOWS__
  // Create executor domain socket if the user so desires.
  Option<Socket> executorSocket = None();
  if (flags.http_executor_domain_sockets) {
    // If `http_executor_domain_sockets` is true, then the location should have
    // been set by the user or automatically during startup.
    CHECK_SOME(flags.domain_socket_location);

    if (strings::startsWith(*flags.domain_socket_location, "systemd:")) {
      LOG(INFO) << "Expecting domain socket to be passed by systemd";

      // Chop off `systemd:` prefix.
      std::string name = flags.domain_socket_location->substr(8);
#ifdef __linux__
      // NOTE: Some systemd versions do not support FileDescriptorName,
      // thus we also need to listen on descriptors with name "unknown",
      // which is used for sockets where no name could be determined.
      Try<std::vector<int>> socketFds =
        systemd::socket_activation::listenFdsWithNames({name, "unknown"});
#else
      Try<std::vector<int>> socketFds =
        Try<std::vector<int>>({}); // Dummy to avoid compile errors.
      EXIT(EXIT_FAILURE)
        << "Systemd socket passing is only supported on linux.";
#endif

      if (socketFds.isError()) {
        EXIT(EXIT_FAILURE)
          << "Could not get passed file descriptors from systemd: "
          << socketFds.error();
      }

      if (socketFds->size() != 1u) {
        EXIT(EXIT_FAILURE)
          << "Expected exactly one socket with name " << name
          << ", got " << socketFds->size() << " instead.";
      }

      int sockfd = socketFds->at(0);

      // Don't use SSLSocketImpl for unix domain sockets.
      Try<Socket> socket = Socket::create(
          sockfd, process::network::internal::SocketImpl::Kind::POLL);

      if (socket.isError()) {
        EXIT(EXIT_FAILURE)
          << "Failed to create domain socket: " << socket.error();
      }

      executorSocket = socket.get();

      // Adjust socket location to point to the *path*, not the systemd
      // identifier.
      auto addr = process::network::convert<process::network::unix::Address>(
          process::network::address(sockfd).get()).get();

      flags.domain_socket_location = addr.path();
    } else {
      Try<Socket> socket =
        common::createDomainSocket(*flags.domain_socket_location);

      if (socket.isError()) {
        EXIT(EXIT_FAILURE)
          << "Failed to create domain socket: " << socket.error();
      }

      executorSocket = socket.get();
    }

    LOG(INFO) << "Using domain socket at " << *flags.domain_socket_location;
  }
#endif // __WINDOWS__

  Slave* slave = new Slave(
      id,
      flags,
      detector,
      containerizer.get(),
      files,
      gc,
      taskStatusUpdateManager,
      resourceEstimator.get(),
      qosController.get(),
      secretGenerator,
      volumeGidManager,
      futureTracker.get(),
      std::move(csiServer),
#ifndef __WINDOWS__
      executorSocket,
#endif // __WINDOWS__
      authorizer_);

  process::spawn(slave);
  process::wait(slave->self());

  delete slave;

  delete secretGenerator;

  delete qosController.get();

  delete resourceEstimator.get();

  delete taskStatusUpdateManager;

  delete files;

  if (authorizer_.isSome()) {
    delete authorizer_.get();
  }

  delete detector;

  delete containerizer.get();

  delete futureTracker.get();

#ifndef __WINDOWS__
  delete volumeGidManager;
#endif // __WINDOWS__

  delete secretResolver.get();

  delete gc;

  delete fetcher;

  // NOTE: We need to finalize libprocess, on Windows especially,
  // as any binary that uses the networking stack on Windows must
  // also clean up the networking stack before exiting.
  process::finalize(true);
  return EXIT_SUCCESS;
}
