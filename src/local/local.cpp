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
#include <sstream>
#include <vector>

#include <stout/fatal.hpp>
#include <stout/foreach.hpp>

#include "local.hpp"

#include "configurator/configuration.hpp"
#include "configurator/configurator.hpp"

#include "detector/detector.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/master.hpp"

#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

using namespace mesos::internal;

using mesos::internal::master::AllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::IsolationModule;
using mesos::internal::slave::ProcessBasedIsolationModule;

using process::PID;
using process::UPID;

using std::map;
using std::string;
using std::stringstream;
using std::vector;


namespace mesos {
namespace internal {
namespace local {

static AllocatorProcess* allocator = NULL;
static Master* master = NULL;
static map<IsolationModule*, Slave*> slaves;
static MasterDetector* detector = NULL;
static Files* files = NULL;


PID<Master> launch(int numSlaves,
                   int32_t cpus,
                   int64_t mem,
                   bool quiet,
                   AllocatorProcess* _allocator)
{
  Configuration configuration;
  configuration.set("slaves", "*");
  configuration.set("num_slaves", numSlaves);
  configuration.set("quiet", quiet);

  stringstream out;
  out << "cpus:" << cpus << ";" << "mem:" << mem;
  configuration.set("resources", out.str());

  return launch(configuration, _allocator);
}


PID<Master> launch(const Configuration& configuration,
                   AllocatorProcess* _allocator)
{
  int numSlaves = configuration.get<int>("num_slaves", 1);

  if (master != NULL) {
    LOG(FATAL) << "Can only launch one local cluster at a time (for now)";
  }

  if (_allocator == NULL) {
    // Create default allocator, save it for deleting later.
    _allocator = allocator = AllocatorProcess::create("drf", "drf");
  } else {
    // TODO(benh): Figure out the behavior of allocator pointer and remove the
    // else block.
    allocator = NULL;
  }

  files = new Files();

  {
    flags::Flags<logging::Flags, master::Flags> flags;
    flags.load(configuration.getMap());
    master = new Master(_allocator, files, flags);
  }

  PID<Master> pid = process::spawn(master);

  vector<UPID> pids;

  flags::Flags<logging::Flags, slave::Flags> flags;
  flags.load(configuration.getMap());

  for (int i = 0; i < numSlaves; i++) {
    // TODO(benh): Create a local isolation module?
    ProcessBasedIsolationModule* isolationModule =
      new ProcessBasedIsolationModule();
    Slave* slave = new Slave(flags, true, isolationModule, files);
    slaves[isolationModule] = slave;
    pids.push_back(process::spawn(slave));
  }

  detector = new BasicMasterDetector(pid, pids, true);

  return pid;
}


void shutdown()
{
  if (master != NULL) {
    process::terminate(master->self());
    process::wait(master->self());
    delete master;
    delete allocator;
    master = NULL;

    // TODO(benh): Ugh! Because the isolation module calls back into the
    // slave (not the best design) we can't delete the slave until we
    // have deleted the isolation module. But since the slave calls into
    // the isolation module, we can't delete the isolation module until
    // we have stopped the slave.

    foreachpair (IsolationModule* isolationModule, Slave* slave, slaves) {
      process::terminate(slave->self());
      process::wait(slave->self());
      delete isolationModule;
      delete slave;
    }

    slaves.clear();

    delete detector;
    detector = NULL;

    delete files;
    files = NULL;
  }
}

} // namespace local {
} // namespace internal {
} // namespace mesos {
