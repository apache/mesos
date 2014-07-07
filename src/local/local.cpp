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

#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>
#include <stout/strings.hpp>

#include "authorizer/authorizer.hpp"

#include "common/protobuf_utils.hpp"

#include "local.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/allocator.hpp"
#include "master/contender.hpp"
#include "master/detector.hpp"
#include "master/drf_sorter.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"
#include "master/registrar.hpp"
#include "master/repairer.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/slave.hpp"

#include "state/in_memory.hpp"
#include "state/log.hpp"
#include "state/protobuf.hpp"
#include "state/storage.hpp"

using namespace mesos::internal;
using namespace mesos::internal::log;

using mesos::internal::master::allocator::Allocator;
using mesos::internal::master::allocator::AllocatorProcess;
using mesos::internal::master::allocator::DRFSorter;
using mesos::internal::master::allocator::HierarchicalDRFAllocatorProcess;

using mesos::internal::master::Master;
using mesos::internal::master::Registrar;
using mesos::internal::master::Repairer;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Slave;

using process::Owned;
using process::PID;
using process::UPID;

using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::vector;


namespace mesos {
namespace internal {
namespace local {

static Allocator* allocator = NULL;
static AllocatorProcess* allocatorProcess = NULL;
static Log* log = NULL;
static state::Storage* storage = NULL;
static state::protobuf::State* state = NULL;
static Registrar* registrar = NULL;
static Repairer* repairer = NULL;
static Master* master = NULL;
static map<Containerizer*, Slave*> slaves;
static StandaloneMasterDetector* detector = NULL;
static MasterContender* contender = NULL;
static Option<Authorizer*> authorizer = None();
static Files* files = NULL;


PID<Master> launch(const Flags& flags, Allocator* _allocator)
{
  if (master != NULL) {
    LOG(FATAL) << "Can only launch one local cluster at a time (for now)";
  }

  if (_allocator == NULL) {
    // Create default allocator, save it for deleting later.
    allocatorProcess = new HierarchicalDRFAllocatorProcess();
    _allocator = allocator = new Allocator(allocatorProcess);
  } else {
    // TODO(benh): Figure out the behavior of allocator pointer and remove the
    // else block.
    allocator = NULL;
    allocatorProcess = NULL;
  }

  files = new Files();

  {
    master::Flags flags;
    Try<Nothing> load = flags.load("MESOS_");
    if (load.isError()) {
      EXIT(1) << "Failed to start a local cluster while loading "
              << "master flags from the environment: " << load.error();
    }

    if (flags.registry == "in_memory") {
      if (flags.registry_strict) {
        EXIT(1) << "Cannot use '--registry_strict' when using in-memory storage"
                << " based registry";
      }
      storage = new state::InMemoryStorage();
    } else if (flags.registry == "replicated_log") {
      // For local runs, we use a temporary work directory.
      if (flags.work_dir.isNone()) {
        CHECK_SOME(os::mkdir("/tmp/mesos/local"));

        Try<string> directory = os::mkdtemp("/tmp/mesos/local/XXXXXX");
        CHECK_SOME(directory);
        flags.work_dir = directory.get();
      }

      // TODO(vinod): Add support for replicated log with ZooKeeper.
      log = new Log(
          1,
          path::join(flags.work_dir.get(), "replicated_log"),
          set<UPID>(),
          flags.log_auto_initialize);
      storage = new state::LogStorage(log);
    } else {
      EXIT(1) << "'" << flags.registry << "' is not a supported"
              << " option for registry persistence";
    }

    CHECK_NOTNULL(storage);

    state = new state::protobuf::State(storage);
    registrar = new Registrar(flags, state);
    repairer = new Repairer();

    contender = new StandaloneMasterContender();
    detector = new StandaloneMasterDetector();

    if (flags.acls.isSome()) {
      Try<Owned<Authorizer> > authorizer_ =
        Authorizer::create(flags.acls.get());

      if (authorizer_.isError()) {
        EXIT(1) << "Failed to initialize the authorizer: "
                << authorizer_.error() << " (see --acls flag)";
      }
      Owned<Authorizer> authorizer__ = authorizer_.get();
      authorizer = authorizer__.release();
    }

    master = new Master(
        _allocator,
        registrar,
        repairer,
        files,
        contender,
        detector,
        authorizer,
        flags);

    detector->appoint(master->info());
  }

  PID<Master> pid = process::spawn(master);

  vector<UPID> pids;

  for (int i = 0; i < flags.num_slaves; i++) {
    slave::Flags flags;
    Try<Nothing> load = flags.load("MESOS_");

    if (load.isError()) {
      EXIT(1) << "Failed to start a local cluster while loading "
              << "slave flags from the environment: " << load.error();
    }

    Try<Containerizer*> containerizer = Containerizer::create(flags, true);
    if (containerizer.isError()) {
      EXIT(1) << "Failed to create a containerizer: " << containerizer.error();
    }

    // Use a different work directory for each slave.
    flags.work_dir = path::join(flags.work_dir, stringify(i));

    // NOTE: At this point detector is already initialized by the
    // Master.
    Slave* slave = new Slave(flags, detector, containerizer.get(), files);
    slaves[containerizer.get()] = slave;
    pids.push_back(process::spawn(slave));
  }

  return pid;
}


void shutdown()
{
  if (master != NULL) {
    process::terminate(master->self());
    process::wait(master->self());
    delete master;
    delete allocator;
    delete allocatorProcess;
    master = NULL;

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

    if (authorizer.isSome()) {
      delete authorizer.get();
      authorizer = None();
    }

    delete detector;
    detector = NULL;

    delete contender;
    contender = NULL;

    delete files;
    files = NULL;

    delete registrar;
    registrar = NULL;

    delete repairer;
    repairer = NULL;

    delete state;
    state = NULL;

    delete storage;
    storage = NULL;

    delete log;
    log = NULL;
  }
}

} // namespace local {
} // namespace internal {
} // namespace mesos {
