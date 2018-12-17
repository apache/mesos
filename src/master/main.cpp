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

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/allocator/allocator.hpp>
#include <mesos/master/contender.hpp>
#include <mesos/master/detector.hpp>

#include <mesos/module/anonymous.hpp>
#include <mesos/module/authorizer.hpp>

#include <mesos/state/in_memory.hpp>
#ifndef __WINDOWS__
#include <mesos/state/log.hpp>
#endif // __WINDOWS__
#include <mesos/state/state.hpp>
#include <mesos/state/storage.hpp>

#include <mesos/zookeeper/detector.hpp>

#include <process/limiter.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/version.hpp>

#include "common/authorization.hpp"
#include "common/build.hpp"
#include "common/http.hpp"
#include "common/protobuf_utils.hpp"

#include "hook/manager.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/master.hpp"
#include "master/registrar.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "master/detector/standalone.hpp"

#include "module/manager.hpp"

#include "version/version.hpp"

using namespace mesos::internal;
#ifndef __WINDOWS__
using namespace mesos::internal::log;
#endif // __WINDOWS__
using namespace mesos::internal::master;
using namespace zookeeper;

using mesos::Authorizer;
using mesos::MasterInfo;
using mesos::Parameter;
using mesos::Parameters;

#ifndef __WINDOWS__
using mesos::log::Log;
#endif // __WINDOWS__

using mesos::allocator::Allocator;

using mesos::master::contender::MasterContender;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::modules::Anonymous;
using mesos::modules::ModuleManager;

using mesos::state::InMemoryStorage;
#ifndef __WINDOWS__
using mesos::state::LogStorage;
#endif // __WINDOWS__
using mesos::state::Storage;

using process::Owned;
using process::RateLimiter;
using process::UPID;

using process::firewall::DisabledEndpointsFirewallRule;
using process::firewall::FirewallRule;

using std::cerr;
using std::cout;
using std::endl;
using std::move;
using std::ostringstream;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;


int main(int argc, char** argv)
{
  // The order of initialization of various master components is as follows:
  // * Validate flags.
  // * Logging.
  // * Log build information.
  // * Libprocess.
  // * Version process.
  // * Firewall rules: should be initialized before initializing HTTP endpoints.
  // * Modules: Load module libraries and manifests before they
  //   can be instantiated.
  // * Anonymous modules: Later components such as Allocators, and master
  //   contender/detector might depend upon anonymous modules.
  // * Hooks.
  // * Allocator.
  // * Registry storage.
  // * State.
  // * Master contender.
  // * Master detector.
  // * Authorizer.
  // * Slave removal rate limiter.
  // * `Master` process.
  //
  // TODO(avinash): Add more comments discussing the rationale behind for this
  // particular component ordering.

  GOOGLE_PROTOBUF_VERIFY_VERSION;

  master::Flags flags;

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
         << "See `mesos-master --help` for a list of supported flags." << endl;
    return EXIT_FAILURE;
  }

  logging::initialize(argv[0], true, flags); // Catch signals.

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  // Check that master's version has the expected format (SemVer).
  {
    Try<Version> version = Version::parse(MESOS_VERSION);
    if (version.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to parse Mesos version '" << MESOS_VERSION << "': "
        << version.error();
    }
  }

  if (flags.ip_discovery_command.isSome() && flags.ip.isSome()) {
    EXIT(EXIT_FAILURE)
      << "Only one of `--ip` or `--ip_discovery_command` should be specified";
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

  if (flags.zk.isNone()) {
    if (flags.master_contender.isSome() ^ flags.master_detector.isSome()) {
      EXIT(EXIT_FAILURE)
        << "Both --master_contender and --master_detector should "
           "be specified or omitted.";
    }
  } else {
    if (flags.master_contender.isSome() || flags.master_detector.isSome()) {
      EXIT(EXIT_FAILURE)
        << "Only one of --zk or the "
           "--master_contender/--master_detector "
           "pair should be specified.";
    }
  }

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

  // This should be the first invocation of `process::initialize`. If it returns
  // `false`, then it has already been called, which means that the
  // authentication realm for libprocess-level HTTP endpoints was not set to the
  // correct value for the master.
  if (!process::initialize(
          "master",
          READWRITE_HTTP_AUTHENTICATION_REALM,
          READONLY_HTTP_AUTHENTICATION_REALM)) {
    EXIT(EXIT_FAILURE) << "The call to `process::initialize()` in the master's "
                       << "`main()` was not the function's first invocation";
  }

  spawn(new VersionProcess(), true);

  // Initialize firewall rules.
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
    // do any housekeeping necessary when the master is cleanly
    // terminating.
  }

  // Initialize hooks.
  if (flags.hooks.isSome()) {
    Try<Nothing> result = HookManager::initialize(flags.hooks.get());
    if (result.isError()) {
      EXIT(EXIT_FAILURE) << "Error installing hooks: " << result.error();
    }
  }

  // Create an instance of allocator.
  Try<Allocator*> allocator = Allocator::create(
      flags.allocator,
      flags.role_sorter,
      flags.framework_sorter);

  if (allocator.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create allocator '" << flags.allocator << "'"
      << ": " << allocator.error();
  }

  CHECK_NOTNULL(allocator.get());
  LOG(INFO) << "Using '" << flags.allocator << "' allocator";

  Storage* storage = nullptr;
#ifndef __WINDOWS__
  Log* log = nullptr;
#endif // __WINDOWS__

  if (flags.registry == "in_memory") {
    storage = new InMemoryStorage();
#ifndef __WINDOWS__
  } else if (flags.registry == "replicated_log" ||
             flags.registry == "log_storage") {
    // TODO(bmahler): "log_storage" is present for backwards
    // compatibility, can be removed before 0.19.0.
    if (flags.work_dir.isNone()) {
      EXIT(EXIT_FAILURE)
        << "--work_dir needed for replicated log based registry";
    }

    Try<Nothing> mkdir = os::mkdir(flags.work_dir.get());
    if (mkdir.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create work directory '" << flags.work_dir.get()
        << "': " << mkdir.error();
    }

    if (flags.zk.isSome()) {
      // Use replicated log with ZooKeeper.
      if (flags.quorum.isNone()) {
        EXIT(EXIT_FAILURE)
          << "Need to specify --quorum for replicated log based"
          << " registry when using ZooKeeper";
      }

      Try<zookeeper::URL> url = zookeeper::URL::parse(flags.zk->value);
      if (url.isError()) {
        EXIT(EXIT_FAILURE) << "Error parsing ZooKeeper URL: " << url.error();
      }

      log = new Log(
          flags.quorum.get(),
          path::join(flags.work_dir.get(), "replicated_log"),
          url->servers,
          flags.zk_session_timeout,
          path::join(url->path, "log_replicas"),
          url->authentication,
          flags.log_auto_initialize,
          "registrar/");
    } else {
      // Use replicated log without ZooKeeper.
      log = new Log(
          1,
          path::join(flags.work_dir.get(), "replicated_log"),
          set<UPID>(),
          flags.log_auto_initialize,
          "registrar/");
    }
    storage = new LogStorage(log);
#endif // __WINDOWS__
  } else {
    EXIT(EXIT_FAILURE)
      << "'" << flags.registry << "' is not a supported"
      << " option for registry persistence";
  }

  CHECK_NOTNULL(storage);

  mesos::state::State* state = new mesos::state::State(storage);
  Registrar* registrar =
    new Registrar(flags, state, READONLY_HTTP_AUTHENTICATION_REALM);

  MasterContender* contender;
  MasterDetector* detector;

  Try<MasterContender*> contender_ = MasterContender::create(
      flags.zk.isSome() ? flags.zk->value : Option<string>::none(),
      flags.master_contender,
      flags.zk_session_timeout);

  if (contender_.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create a master contender: " << contender_.error();
  }

  contender = contender_.get();

  Try<MasterDetector*> detector_ = MasterDetector::create(
      flags.zk.isSome() ? flags.zk->value : Option<string>::none(),
      flags.master_detector,
      flags.zk_session_timeout);

  if (detector_.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create a master detector: " << detector_.error();
  }

  detector = detector_.get();

  Option<Authorizer*> authorizer_ = None();

  auto authorizerNames = strings::split(flags.authorizers, ",");
  if (authorizerNames.empty()) {
    EXIT(EXIT_FAILURE) << "No authorizer specified";
  }
  if (authorizerNames.size() > 1) {
    EXIT(EXIT_FAILURE) << "Multiple authorizers not supported";
  }
  string authorizerName = authorizerNames[0];

  // NOTE: The flag --authorizers overrides the flag --acls, i.e. if
  // a non default authorizer is requested, it will be used and
  // the contents of --acls will be ignored.
  // TODO(arojas): Consider adding support for multiple authorizers.
  Result<Authorizer*> authorizer((None()));
  if (authorizerName != master::DEFAULT_AUTHORIZER) {
    LOG(INFO) << "Creating '" << authorizerName << "' authorizer";

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
    // Note that these callbacks capture `authorizer_.get()`, but the master
    // creates a copy of the authorizer during construction. Thus, if in the
    // future it becomes possible to dynamically set the authorizer, this would
    // break.
    process::http::authorization::setCallbacks(
        mesos::authorization::createAuthorizationCallbacks(authorizer_.get()));
  }

  Files files(READONLY_HTTP_AUTHENTICATION_REALM, authorizer_);

  Option<shared_ptr<RateLimiter>> slaveRemovalLimiter = None();
  if (flags.agent_removal_rate_limit.isSome()) {
    // Parse the flag value.
    // TODO(vinod): Move this parsing logic to flags once we have a
    // 'Rate' abstraction in stout.
    vector<string> tokens =
      strings::tokenize(flags.agent_removal_rate_limit.get(), "/");

    if (tokens.size() != 2) {
      EXIT(EXIT_FAILURE)
        << "Invalid agent_removal_rate_limit: "
        << flags.agent_removal_rate_limit.get()
        << ". Format is <Number of agents>/<Duration>";
    }

    Try<int> permits = numify<int>(tokens[0]);
    if (permits.isError()) {
      EXIT(EXIT_FAILURE)
        << "Invalid agent_removal_rate_limit: "
        << flags.agent_removal_rate_limit.get()
        << ". Format is <Number of agents>/<Duration>"
        << ": " << permits.error();
    }

    Try<Duration> duration = Duration::parse(tokens[1]);
    if (duration.isError()) {
      EXIT(EXIT_FAILURE)
        << "Invalid agent_removal_rate_limit: "
        << flags.agent_removal_rate_limit.get()
        << ". Format is <Number of agents>/<Duration>"
        << ": " << duration.error();
    }

    slaveRemovalLimiter = new RateLimiter(permits.get(), duration.get());
  }

  Master* master =
    new Master(
      allocator.get(),
      registrar,
      &files,
      contender,
      detector,
      authorizer_,
      slaveRemovalLimiter,
      flags);

  if (flags.zk.isNone() && flags.master_detector.isNone()) {
    // It means we are using the standalone detector so we need to
    // appoint this Master as the leader.
    dynamic_cast<StandaloneMasterDetector*>(detector)->appoint(master->info());
  }

  process::spawn(master);
  process::wait(master->self());

  delete master;
  delete allocator.get();

  delete registrar;
  delete state;
  delete storage;
#ifndef __WINDOWS__
  delete log;
#endif // __WINDOWS__

  delete contender;
  delete detector;

  if (authorizer_.isSome()) {
    delete authorizer_.get();
  }

  return EXIT_SUCCESS;
}
