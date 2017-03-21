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

#include <mesos/slave/resource_estimator.hpp>

#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/flags.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>

#ifdef __linux__
#include <stout/proc.hpp>
#endif // __linux__

#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/build.hpp"
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

#include "slave/gc.hpp"
#include "slave/slave.hpp"
#include "slave/status_update_manager.hpp"

#include "version/version.hpp"

using namespace mesos::internal;
using namespace mesos::internal::slave;

using mesos::master::detector::MasterDetector;

using mesos::modules::Anonymous;
using mesos::modules::ModuleManager;

using mesos::master::detector::MasterDetector;

using mesos::slave::QoSController;
using mesos::slave::ResourceEstimator;

using mesos::Authorizer;
using mesos::SlaveInfo;

using process::Owned;

using process::firewall::DisabledEndpointsFirewallRule;
using process::firewall::FirewallRule;

using std::cerr;
using std::cout;
using std::endl;
using std::move;
using std::set;
using std::string;
using std::vector;


class Flags : public virtual slave::Flags
{
public:
  Flags()
  {
    add(&Flags::ip,
        "ip",
        "IP address to listen on. This cannot be used in conjunction\n"
        "with `--ip_discovery_command`.");

    add(&Flags::port, "port", "Port to listen on.", SlaveInfo().port());

    add(&Flags::advertise_ip,
        "advertise_ip",
        "IP address advertised to reach this Mesos slave.\n"
        "The slave does not bind to this IP address.\n"
        "However, this IP address may be used to access this slave.");

    add(&Flags::advertise_port,
        "advertise_port",
        "Port advertised to reach this Mesos slave (along with\n"
        "`advertise_ip`). The slave does not bind to this port.\n"
        "However, this port (along with `advertise_ip`) may be used to\n"
        "access this slave.");

    add(&Flags::master,
        "master",
        "May be one of:\n"
        "  `host:port`\n"
        "  `zk://host1:port1,host2:port2,.../path`\n"
        "  `zk://username:password@host1:port1,host2:port2,.../path`\n"
        "  `file:///path/to/file` (where file contains one of the above)");


    add(&Flags::ip_discovery_command,
        "ip_discovery_command",
        "Optional IP discovery binary: if set, it is expected to emit\n"
        "the IP address which the slave will try to bind to.\n"
        "Cannot be used in conjunction with `--ip`.");
  }

  // The following flags are executable specific (e.g., since we only
  // have one instance of libprocess per execution, we only want to
  // advertise the IP and port option once, here).

  Option<string> ip;
  uint16_t port;
  Option<string> advertise_ip;
  Option<string> advertise_port;
  Option<string> master;

  // Optional IP discover script that will set the slave's IP.
  // If set, its output is expected to be a valid parseable IP string.
  Option<string> ip_discovery_command;
};


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
static Try<Nothing> assignCgroups(const ::Flags& flags)
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

    Try<bool> exists = cgroups::exists(hierarchy.get(), cgroup);
    if (exists.isError()) {
      return Error(
          "Failed to find cgroup " + cgroup +
          " for subsystem " + subsystem +
          " under hierarchy " + hierarchy.get() +
          " for agent: " + exists.error());
    }

    if (!exists.get()) {
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
    if (!processes.get().empty()) {
      // For each process, we print its pid as well as its command
      // to help triaging.
      vector<string> infos;
      foreach (pid_t pid, processes.get()) {
        Result<os::Process> proc = os::process(pid);

        // Only print the command if available.
        if (proc.isSome()) {
          infos.push_back(stringify(pid) + " '" + proc.get().command + "'");
        } else {
          infos.push_back(stringify(pid));
        }
      }

      LOG(INFO) << "An agent (or child process) is still running, please"
                << " consider checking the following process(es) listed in "
                << path::join(hierarchy.get(), cgroup, "cgroups.proc")
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
  // * Log build information.
  // * Libprocess
  // * Logging
  // * Version process
  // * Firewall rules: should be initialized before initializing HTTP endpoints.
  // * Modules: Load module libraries and manifests before they
  //   can be instantiated.
  // * Anonymous modules: Later components such as Allocators, and master
  //   contender/detector might depend upon anonymous modules.
  // * Hooks.
  // * Systemd support (if it exists).
  // * Fetcher and Containerizer.
  // * Master detector.
  // * Authorizer.
  // * Garbage collector.
  // * Status update manager.
  // * Resource estimator.
  // * QoS controller.
  // * `Agent` process.
  //
  // TODO(avinash): Add more comments discussing the rationale behind for this
  // particular component ordering.

  GOOGLE_PROTOBUF_VERIFY_VERSION;

  ::Flags flags;

  Try<flags::Warnings> load = flags.load("MESOS_", argc, argv);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (flags.version) {
    cout << "mesos" << " " << MESOS_VERSION << endl;
    return EXIT_SUCCESS;
  }

  // TODO(marco): this pattern too should be abstracted away
  // in FlagsBase; I have seen it at least 15 times.
  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  if (flags.master.isNone() && flags.master_detector.isNone()) {
    cerr << flags.usage("Missing required option `--master` or "
                        "`--master_detector`.") << endl;
    return EXIT_FAILURE;
  }

  if (flags.master.isSome() && flags.master_detector.isSome()) {
    cerr << flags.usage("Only one of --master or --master_detector options "
                        "should be specified.");
    return EXIT_FAILURE;
  }

  // Initialize libprocess.
  if (flags.ip_discovery_command.isSome() && flags.ip.isSome()) {
    EXIT(EXIT_FAILURE) << flags.usage(
        "Only one of `--ip` or `--ip_discovery_command` should be specified");
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

  os::setenv("LIBPROCESS_PORT", stringify(flags.port));

  if (flags.advertise_ip.isSome()) {
    os::setenv("LIBPROCESS_ADVERTISE_IP", flags.advertise_ip.get());
  }

  if (flags.advertise_port.isSome()) {
    os::setenv("LIBPROCESS_ADVERTISE_PORT", flags.advertise_port.get());
  }

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

  logging::initialize(argv[0], flags, true); // Catch signals.

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
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
      flags.usage("Only one of --modules or --modules_dir should be specified");
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
    LOG(INFO) << "Inializing systemd state";

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

  Fetcher* fetcher = new Fetcher();

  Try<Containerizer*> containerizer =
    Containerizer::create(flags, false, fetcher);

  if (containerizer.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create a containerizer: " << containerizer.error();
  }

  Try<MasterDetector*> detector_ = MasterDetector::create(
      flags.master, flags.master_detector);

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
        createAuthorizationCallbacks(authorizer_.get()));
  }

  Files* files = new Files(READONLY_HTTP_AUTHENTICATION_REALM, authorizer_);
  GarbageCollector* gc = new GarbageCollector();
  StatusUpdateManager* statusUpdateManager = new StatusUpdateManager(flags);

  Try<ResourceEstimator*> resourceEstimator =
    ResourceEstimator::create(flags.resource_estimator);

  if (resourceEstimator.isError()) {
    cerr << "Failed to create resource estimator: "
         << resourceEstimator.error() << endl;
    return EXIT_FAILURE;
  }

  Try<QoSController*> qosController =
    QoSController::create(flags.qos_controller);

  if (qosController.isError()) {
    cerr << "Failed to create QoS Controller: "
         << qosController.error() << endl;
    return EXIT_FAILURE;
  }

  Slave* slave = new Slave(
      id,
      flags,
      detector,
      containerizer.get(),
      files,
      gc,
      statusUpdateManager,
      resourceEstimator.get(),
      qosController.get(),
      authorizer_);

  process::spawn(slave);
  process::wait(slave->self());

  delete slave;

  delete qosController.get();

  delete resourceEstimator.get();

  delete statusUpdateManager;

  delete gc;

  delete files;

  if (authorizer_.isSome()) {
    delete authorizer_.get();
  }

  delete detector;

  delete containerizer.get();

  delete fetcher;

  // NOTE: We need to finalize libprocess, on Windows especially,
  // as any binary that uses the networking stack on Windows must
  // also clean up the networking stack before exiting.
  process::finalize(true);
  return EXIT_SUCCESS;
}
