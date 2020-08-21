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

#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/allocator/allocator.hpp>

#include <mesos/module/anonymous.hpp>
#include <mesos/module/authorizer.hpp>
#include <mesos/module/contender.hpp>
#include <mesos/module/detector.hpp>
#include <mesos/module/secret_resolver.hpp>

#include <mesos/secret/resolver.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include <mesos/state/in_memory.hpp>
#ifndef __WINDOWS__
#include <mesos/state/log.hpp>
#endif // __WINDOWS__
#include <mesos/state/protobuf.hpp>
#include <mesos/state/storage.hpp>

#include <process/limiter.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>
#include <stout/strings.hpp>

#ifdef USE_SSL_SOCKET
#include <stout/os/permissions.hpp>
#endif // USE_SSL_SOCKET

#ifdef USE_SSL_SOCKET
#include "authentication/executor/jwt_secret_generator.hpp"
#endif // USE_SSL_SOCKET

#include "common/protobuf_utils.hpp"

#include "local.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/master.hpp"
#include "master/registrar.hpp"

#include "master/allocator/mesos/hierarchical.hpp"
#include "master/allocator/mesos/sorter/drf/sorter.hpp"

#include "master/contender/standalone.hpp"

#include "master/detector/standalone.hpp"

#include "module/manager.hpp"

#include "slave/gc.hpp"
#include "slave/slave.hpp"
#include "slave/task_status_update_manager.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

using namespace mesos::internal;
#ifndef __WINDOWS__
using namespace mesos::internal::log;
#endif // __WINDOWS__

using mesos::SecretGenerator;

#ifndef __WINDOWS__
using mesos::log::Log;
#endif // __WINDOWS__

using mesos::allocator::Allocator;

#ifdef USE_SSL_SOCKET
using mesos::authentication::executor::JWTSecretGenerator;
#endif // USE_SSL_SOCKET

using mesos::master::contender::MasterContender;
using mesos::master::contender::StandaloneMasterContender;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using mesos::internal::master::Master;
using mesos::internal::master::Registrar;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::GarbageCollector;
using mesos::internal::slave::Slave;
using mesos::internal::slave::TaskStatusUpdateManager;

using mesos::modules::Anonymous;
using mesos::modules::ModuleManager;

using mesos::slave::QoSController;
using mesos::slave::ResourceEstimator;

using process::Owned;
using process::PID;
using process::RateLimiter;
using process::UPID;

using std::map;
using std::set;
using std::shared_ptr;
using std::string;
using std::stringstream;
using std::vector;


namespace mesos {
namespace internal {
namespace local {

static Allocator* allocator = nullptr;
#ifndef __WINDOWS__
static Log* log = nullptr;
#endif // __WINDOWS__
static mesos::state::Storage* storage = nullptr;
static mesos::state::protobuf::State* state = nullptr;
static Registrar* registrar = nullptr;
static Master* master = nullptr;
static map<Containerizer*, Slave*> slaves;
static StandaloneMasterDetector* detector = nullptr;
static MasterContender* contender = nullptr;
static Option<Authorizer*> authorizer_ = None();
static Files* files = nullptr;
static vector<GarbageCollector*>* garbageCollectors = nullptr;
static vector<TaskStatusUpdateManager*>* taskStatusUpdateManagers = nullptr;
static vector<Fetcher*>* fetchers = nullptr;
static vector<ResourceEstimator*>* resourceEstimators = nullptr;
static vector<QoSController*>* qosControllers = nullptr;
static vector<SecretGenerator*>* secretGenerators = nullptr;
static vector<SecretResolver*>* secretResolvers = nullptr;


PID<Master> launch(const Flags& flags, Allocator* _allocator)
{
  if (master != nullptr) {
    LOG(FATAL) << "Can only launch one local cluster at a time (for now)";
  }

  if (_allocator == nullptr) {
    // Create a default allocator.
    Try<Allocator*> defaultAllocator = HierarchicalDRFAllocator::create();
    if (defaultAllocator.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create an instance of HierarchicalDRFAllocator: "
        << defaultAllocator.error();
    }

    // Update caller's instance.
    _allocator = defaultAllocator.get();

    // Save the instance for deleting later.
    allocator = defaultAllocator.get();
  } else {
    // TODO(benh): Figure out the behavior of allocator pointer and
    // remove the else block.
    allocator = nullptr;
  }

  files = new Files();

  {
    // Use to propagate necessary flags to master. Without this, the
    // load call will spit out an error unless their corresponding
    // environment variables explicitly set.
    map<string, string> propagatedFlags;
    propagatedFlags["work_dir"] = path::join(flags.work_dir, "master");

    master::Flags masterFlags;
    Try<flags::Warnings> load = masterFlags.load(
        propagatedFlags,
        false,
        "MESOS_");

    if (load.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to start a local cluster while loading"
        << " master flags from the environment: " << load.error();
    }

    // Log any flag warnings.
    foreach (const flags::Warning& warning, load->warnings) {
      LOG(WARNING) << warning.message;
    }

    // Attempt to create the `work_dir` for master as a convenience.
    Try<Nothing> mkdir = os::mkdir(masterFlags.work_dir.get());
    if (mkdir.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create the work_dir for master '"
        << masterFlags.work_dir.get() << "': " << mkdir.error();
    }

    // Load modules. Note that this covers both, master and slave
    // specific modules as both use the same flag (--modules).
    if (masterFlags.modules.isSome()) {
      Try<Nothing> result = ModuleManager::load(masterFlags.modules.get());
      if (result.isError()) {
        EXIT(EXIT_FAILURE) << "Error loading modules: " << result.error();
      }
    }

    if (masterFlags.registry == "in_memory") {
      storage = new mesos::state::InMemoryStorage();
#ifndef __WINDOWS__
    } else if (masterFlags.registry == "replicated_log") {
      // TODO(vinod): Add support for replicated log with ZooKeeper.
      log = new Log(
          1,
          path::join(masterFlags.work_dir.get(), "replicated_log"),
          set<UPID>(),
          masterFlags.log_auto_initialize,
          "registrar/");
      storage = new mesos::state::LogStorage(log);
#endif // __WINDOWS__
    } else {
      EXIT(EXIT_FAILURE)
        << "'" << masterFlags.registry << "' is not a supported"
        << " option for registry persistence";
    }

    CHECK_NOTNULL(storage);

    state = new mesos::state::protobuf::State(storage);
    registrar = new Registrar(
        masterFlags,
        state,
        master::READONLY_HTTP_AUTHENTICATION_REALM);

    contender = new StandaloneMasterContender();
    detector = new StandaloneMasterDetector();

    auto authorizerNames = strings::split(masterFlags.authorizers, ",");
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
      if (masterFlags.acls.isSome()) {
        LOG(INFO) << "Creating default '" << authorizerName << "' authorizer";

        authorizer = Authorizer::create(masterFlags.acls.get());
      }
    }

    if (authorizer.isError()) {
      EXIT(EXIT_FAILURE) << "Could not create '" << authorizerName
                         << "' authorizer: " << authorizer.error();
    } else if (authorizer.isSome()) {
      authorizer_ = authorizer.get();
    }

    Option<shared_ptr<RateLimiter>> slaveRemovalLimiter = None();
    if (masterFlags.agent_removal_rate_limit.isSome()) {
      // Parse the flag value.
      // TODO(vinod): Move this parsing logic to flags once we have a
      // 'Rate' abstraction in stout.
      vector<string> tokens =
        strings::tokenize(masterFlags.agent_removal_rate_limit.get(), "/");

      if (tokens.size() != 2) {
        EXIT(EXIT_FAILURE)
          << "Invalid agent_removal_rate_limit: "
          << masterFlags.agent_removal_rate_limit.get()
          << ". Format is <Number of agents>/<Duration>";
      }

      Try<int> permits = numify<int>(tokens[0]);
      if (permits.isError()) {
        EXIT(EXIT_FAILURE)
          << "Invalid agent_removal_rate_limit: "
          << masterFlags.agent_removal_rate_limit.get()
          << ". Format is <Number of agents>/<Duration>"
          << ": " << permits.error();
      }

      Try<Duration> duration = Duration::parse(tokens[1]);
      if (duration.isError()) {
        EXIT(EXIT_FAILURE)
          << "Invalid agent_removal_rate_limit: "
          << masterFlags.agent_removal_rate_limit.get()
          << ". Format is <Number of agents>/<Duration>"
          << ": " << duration.error();
      }

      slaveRemovalLimiter = new RateLimiter(permits.get(), duration.get());
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

    master = new Master(
        _allocator,
        registrar,
        files,
        contender,
        detector,
        authorizer_,
        slaveRemovalLimiter,
        masterFlags);

    detector->appoint(master->info());
  }

  PID<Master> pid = process::spawn(master);

  garbageCollectors = new vector<GarbageCollector*>();
  taskStatusUpdateManagers = new vector<TaskStatusUpdateManager*>();
  fetchers = new vector<Fetcher*>();
  resourceEstimators = new vector<ResourceEstimator*>();
  qosControllers = new vector<QoSController*>();
  secretGenerators = new vector<SecretGenerator*>();
  secretResolvers = new vector<SecretResolver*>();

  vector<UPID> pids;

  for (int i = 0; i < flags.num_slaves; i++) {
    // Use to propagate necessary flags to agent. Without this, the
    // load call will spit out an error unless their corresponding
    // environment variables explicitly set.
    map<string, string> propagatedFlags;

    // Use a different work/runtime/fetcher-cache directory for each agent.
    propagatedFlags["work_dir"] =
      path::join(flags.work_dir, "agents", stringify(i), "work");

    propagatedFlags["runtime_dir"] =
      path::join(flags.work_dir, "agents", stringify(i), "run");

    propagatedFlags["fetcher_cache_dir"] =
      path::join(flags.work_dir, "agents", stringify(i), "fetch");

    slave::Flags slaveFlags;
    Try<flags::Warnings> load = slaveFlags.load(
        propagatedFlags,
        false,
        "MESOS_");

    if (load.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to start a local cluster while loading"
        << " agent flags from the environment: " << load.error();
    }

    // Log any flag warnings (after logging is initialized).
    foreach (const flags::Warning& warning, load->warnings) {
      LOG(WARNING) << warning.message;
    }

    // Attempt to create the `work_dir` for agent as a convenience.
    Try<Nothing> mkdir = os::mkdir(slaveFlags.work_dir);
    if (mkdir.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create the work_dir for agent '"
        << slaveFlags.work_dir << "': " << mkdir.error();
    }

    // Attempt to create the `runtime_dir` for agent as a convenience.
    mkdir = os::mkdir(slaveFlags.runtime_dir);
    if (mkdir.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create the runtime_dir for agent '"
        << slaveFlags.runtime_dir << "': " << mkdir.error();
    }

    garbageCollectors->push_back(new GarbageCollector(slaveFlags.work_dir));
    taskStatusUpdateManagers->push_back(
        new TaskStatusUpdateManager(slaveFlags));
    fetchers->push_back(new Fetcher(slaveFlags));

    Try<ResourceEstimator*> resourceEstimator =
      ResourceEstimator::create(slaveFlags.resource_estimator);

    if (resourceEstimator.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create resource estimator: " << resourceEstimator.error();
    }

    resourceEstimators->push_back(resourceEstimator.get());

    Try<QoSController*> qosController =
      QoSController::create(slaveFlags.qos_controller);

    if (qosController.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create QoS Controller: " << qosController.error();
    }

    qosControllers->push_back(qosController.get());

    SecretGenerator* secretGenerator = nullptr;

#ifdef USE_SSL_SOCKET
    if (slaveFlags.jwt_secret_key.isSome()) {
      Try<string> jwtSecretKey = os::read(slaveFlags.jwt_secret_key.get());
      if (jwtSecretKey.isError()) {
        EXIT(EXIT_FAILURE) << "Failed to read the file specified by "
                           << "--jwt_secret_key";
      }

      // TODO(greggomann): Factor the following code out into a common helper,
      // since we also do this when loading credentials.
      Try<os::Permissions> permissions =
        os::permissions(slaveFlags.jwt_secret_key.get());
      if (permissions.isError()) {
        LOG(WARNING) << "Failed to stat jwt secret key file '"
                     << slaveFlags.jwt_secret_key.get()
                     << "': " << permissions.error();
      } else if (permissions->others.rwx) {
        LOG(WARNING) << "Permissions on executor secret key file '"
                     << slaveFlags.jwt_secret_key.get()
                     << "' are too open; it is recommended that your"
                     << " key file is NOT accessible by others";
      }

      secretGenerator = new JWTSecretGenerator(jwtSecretKey.get());
    }
#endif // USE_SSL_SOCKET

    secretGenerators->push_back(secretGenerator);

    // Override the default launcher that gets created per agent to
    // 'posix' if we're creating multiple agents because the
    // LinuxLauncher does not support multiple agents on the same host
    // (see MESOS-3793).
    if (flags.num_slaves > 1 && slaveFlags.launcher != "posix") {
      // TODO(benh): This log line will print out for each slave!
      LOG(WARNING) << "Using the 'posix' launcher instead of '"
                   << slaveFlags.launcher << "' since currently only the "
                   << "'posix' launcher supports multiple agents per host";
      slaveFlags.launcher = "posix";
    }

    // Initialize SecretResolver.
    Try<SecretResolver*> secretResolver =
      mesos::SecretResolver::create(slaveFlags.secret_resolver);

    if (secretResolver.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to initialize secret resolver: " << secretResolver.error();
    }

    secretResolvers->push_back(secretResolver.get());

    Try<Containerizer*> containerizer = Containerizer::create(
        slaveFlags,
        true,
        fetchers->back(),
        garbageCollectors->back(),
        secretResolver.get());

    if (containerizer.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create a containerizer: " << containerizer.error();
    }

    // NOTE: At this point detector is already initialized by the
    // Master.
    Slave* slave = new Slave(
        process::ID::generate("slave"),
        slaveFlags,
        detector,
        containerizer.get(),
        files,
        garbageCollectors->back(),
        taskStatusUpdateManagers->back(),
        resourceEstimators->back(),
        qosControllers->back(),
        secretGenerators->back(),
        nullptr,
        nullptr,
        nullptr,
#ifndef __WINDOWS__
        None(),
#endif // __WINDOWS__
        authorizer_); // Same authorizer as master.

    slaves[containerizer.get()] = slave;

    pids.push_back(process::spawn(slave));
  }

  return pid;
}


void shutdown()
{
  if (master != nullptr) {
    process::terminate(master->self());
    process::wait(master->self());
    delete master;
    delete allocator;
    master = nullptr;

    // TODO(benh): Ugh! Because the isolator calls back into the slave
    // (not the best design) we can't delete the slave until we have
    // deleted the isolator. But since the slave calls into the
    // isolator, we can't delete the isolator until we have stopped
    // the slave.

    foreachpair (Containerizer* containerizer, Slave* slave, slaves) {
      process::terminate(slave->self());
      process::wait(slave->self());
      delete containerizer;
      delete slave;
    }

    slaves.clear();

    if (authorizer_.isSome()) {
      delete authorizer_.get();
      authorizer_ = None();
    }

    delete detector;
    detector = nullptr;

    delete contender;
    contender = nullptr;

    delete files;
    files = nullptr;

    foreach (GarbageCollector* gc, *garbageCollectors) {
      delete gc;
    }

    delete garbageCollectors;
    garbageCollectors = nullptr;

    foreach (
        TaskStatusUpdateManager* taskStatusUpdateManager,
        *taskStatusUpdateManagers) {
      delete taskStatusUpdateManager;
    }

    delete taskStatusUpdateManagers;
    taskStatusUpdateManagers = nullptr;

    foreach (Fetcher* fetcher, *fetchers) {
      delete fetcher;
    }

    delete fetchers;
    fetchers = nullptr;

    foreach (SecretResolver* secretResolver, *secretResolvers) {
      delete secretResolver;
    }

    foreach (SecretGenerator* secretGenerator, *secretGenerators) {
      delete secretGenerator;
    }

    delete secretGenerators;
    secretGenerators = nullptr;

    foreach (ResourceEstimator* estimator, *resourceEstimators) {
      delete estimator;
    }

    delete resourceEstimators;
    resourceEstimators = nullptr;

    foreach (QoSController* controller, *qosControllers) {
      delete controller;
    }

    delete qosControllers;
    qosControllers = nullptr;

    delete registrar;
    registrar = nullptr;

    delete state;
    state = nullptr;

    delete storage;
    storage = nullptr;

#ifndef __WINDOWS__
    delete log;
    log = nullptr;
#endif // __WINDOWS__
  }
}

} // namespace local {
} // namespace internal {
} // namespace mesos {
