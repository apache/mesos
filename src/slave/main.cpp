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

#include <stdint.h>

#include <vector>
#include <utility>

#include <mesos/mesos.hpp>

#include <mesos/module/anonymous.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include <process/owned.hpp>

#include <stout/check.hpp>
#include <stout/flags.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/build.hpp"

#include "hook/manager.hpp"

#ifdef __linux__
#include "linux/systemd.hpp"
#endif // __linux__

#include "logging/logging.hpp"

#include "master/detector.hpp"

#include "messages/flags.hpp"
#include "messages/messages.hpp"

#include "module/manager.hpp"

#include "slave/gc.hpp"
#include "slave/slave.hpp"
#include "slave/status_update_manager.hpp"

using namespace mesos::internal;
using namespace mesos::internal::slave;

using mesos::modules::Anonymous;
using mesos::modules::ModuleManager;

using mesos::slave::QoSController;
using mesos::slave::ResourceEstimator;

using mesos::SlaveInfo;

using process::Owned;

using process::firewall::DisabledEndpointsFirewallRule;
using process::firewall::FirewallRule;

using std::cerr;
using std::cout;
using std::endl;
using std::move;
using std::string;
using std::vector;


void version()
{
  cout << "mesos" << " " << MESOS_VERSION << endl;
}


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  slave::Flags flags;

  // The following flags are executable specific (e.g., since we only
  // have one instance of libprocess per execution, we only want to
  // advertise the IP and port option once, here).
  Option<string> ip;
  flags.add(&ip, "ip", "IP address to listen on");

  uint16_t port;
  flags.add(&port, "port", "Port to listen on", SlaveInfo().port());

  Option<string> master;
  flags.add(&master,
            "master",
            "May be one of:\n"
            "  zk://host1:port1,host2:port2,.../path\n"
            "  zk://username:password@host1:port1,host2:port2,.../path\n"
            "  file:///path/to/file (where file contains one of the above)");


  // Optional IP discover script that will set the slave's IP.
  // If set, its output is expected to be a valid parseable IP string.
  Option<string> ip_discovery_command;
  flags.add(&ip_discovery_command,
      "ip_discovery_command",
      "Optional IP discovery binary: if set, it is expected to emit\n"
      "the IP address which slave will try to bind to.\n"
      "Cannot be used in conjunction with --ip.");

  Try<Nothing> load = flags.load("MESOS_", argc, argv);

  // TODO(marco): this pattern too should be abstracted away
  // in FlagsBase; I have seen it at least 15 times.
  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (flags.version) {
    version();
    return EXIT_SUCCESS;
  }

  if (master.isNone()) {
    cerr << flags.usage("Missing required option --master") << endl;
    return EXIT_FAILURE;
  }

  // Initialize modules. Note that since other subsystems may depend
  // upon modules, we should initialize modules before anything else.
  if (flags.modules.isSome()) {
    Try<Nothing> result = ModuleManager::load(flags.modules.get());
    if (result.isError()) {
      EXIT(EXIT_FAILURE) << "Error loading modules: " << result.error();
    }
  }

  // Initialize hooks.
  if (flags.hooks.isSome()) {
    Try<Nothing> result = HookManager::initialize(flags.hooks.get());
    if (result.isError()) {
      EXIT(EXIT_FAILURE) << "Error installing hooks: " << result.error();
    }
  }

  // Initialize libprocess.
  if (ip_discovery_command.isSome() && ip.isSome()) {
    EXIT(EXIT_FAILURE) << flags.usage(
        "Only one of --ip or --ip_discovery_command should be specified");
  }

  if (ip_discovery_command.isSome()) {
    Try<string> ipAddress = os::shell(ip_discovery_command.get());

    if (ipAddress.isError()) {
      EXIT(EXIT_FAILURE) << ipAddress.error();
    }

    os::setenv("LIBPROCESS_IP", strings::trim(ipAddress.get()));
  } else if (ip.isSome()) {
    os::setenv("LIBPROCESS_IP", ip.get());
  }

  os::setenv("LIBPROCESS_PORT", stringify(port));

  process::initialize("slave(1)");

  logging::initialize(argv[0], flags, true); // Catch signals.

  LOG(INFO) << "Build: " << build::DATE << " by " << build::USER;

  LOG(INFO) << "Version: " << MESOS_VERSION;

  if (build::GIT_TAG.isSome()) {
    LOG(INFO) << "Git tag: " << build::GIT_TAG.get();
  }

  if (build::GIT_SHA.isSome()) {
    LOG(INFO) << "Git SHA: " << build::GIT_SHA.get();
  }

  Fetcher fetcher;

#ifdef __linux__
  // Initialize systemd if it exists.
  if (systemd::exists()) {
    LOG(INFO) << "Inializing systemd state";

    systemd::Flags systemdFlags;
    systemdFlags.runtime_directory = flags.systemd_runtime_directory;
    systemdFlags.cgroups_hierarchy = flags.cgroups_hierarchy;

    Try<Nothing> initialize = systemd::initialize(systemdFlags);
    if (initialize.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to initialize systemd: " + initialize.error();
    }
  }
#endif // __linux__

  Try<Containerizer*> containerizer =
    Containerizer::create(flags, false, &fetcher);

  if (containerizer.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create a containerizer: " << containerizer.error();
  }

  Try<MasterDetector*> detector = MasterDetector::create(master.get());

  if (detector.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create a master detector: " << detector.error();
  }

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

  // Create anonymous modules.
  foreach (const string& name, ModuleManager::find<Anonymous>()) {
    Try<Anonymous*> create = ModuleManager::create<Anonymous>(name);
    if (create.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create anonymous module named '" << name << "'";
    }

    // We don't bother keeping around the pointer to this anonymous
    // module, when we exit that will effectively free it's memory.
    //
    // TODO(benh): We might want to add explicit finalization (and
    // maybe explicit initialization too) in order to let the module
    // do any housekeeping necessary when the slave is cleanly
    // terminating.
  }

  Files files;
  GarbageCollector gc;
  StatusUpdateManager statusUpdateManager(flags);

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


  LOG(INFO) << "Starting Mesos slave";

  Slave* slave = new Slave(
      flags,
      detector.get(),
      containerizer.get(),
      &files,
      &gc,
      &statusUpdateManager,
      resourceEstimator.get(),
      qosController.get());

  process::spawn(slave);
  process::wait(slave->self());

  delete slave;

  delete resourceEstimator.get();

  delete qosController.get();

  delete detector.get();

  delete containerizer.get();

  return EXIT_SUCCESS;
}
