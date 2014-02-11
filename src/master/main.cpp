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

#include <mesos/mesos.hpp>

#include <stout/check.hpp>
#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "common/build.hpp"
#include "common/protobuf_utils.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/allocator.hpp"
#include "master/contender.hpp"
#include "master/detector.hpp"
#include "master/drf_sorter.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"
#include "master/registrar.hpp"

#include "state/leveldb.hpp"
#include "state/protobuf.hpp"
#include "state/storage.hpp"


#include "zookeeper/detector.hpp"

using namespace mesos::internal;
using namespace mesos::internal::master;
using namespace zookeeper;

using mesos::MasterInfo;

using std::cerr;
using std::endl;
using std::string;


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << os::basename(argv0).get() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  master::Flags flags;

  // The following flags are executable specific (e.g., since we only
  // have one instance of libprocess per execution, we only want to
  // advertise the IP and port option once, here).
  Option<string> ip;
  flags.add(&ip, "ip", "IP address to listen on");

  uint16_t port;
  flags.add(&port, "port", "Port to listen on", MasterInfo().port());

  string zk;
  flags.add(&zk,
            "zk",
            "ZooKeeper URL (used for leader election amongst masters)\n"
            "May be one of:\n"
            "  zk://host1:port1,host2:port2,.../path\n"
            "  zk://username:password@host1:port1,host2:port2,.../path\n"
            "  file://path/to/file (where file contains one of the above)",
            "");

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  Try<Nothing> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    exit(1);
  }

  if (help) {
    usage(argv[0], flags);
    exit(1);
  }

  // Initialize libprocess.
  if (ip.isSome()) {
    os::setenv("LIBPROCESS_IP", ip.get());
  }

  os::setenv("LIBPROCESS_PORT", stringify(port));

  process::initialize("master");

  logging::initialize(argv[0], flags, true); // Catch signals.

  LOG(INFO) << "Build: " << build::DATE << " by " << build::USER;

  LOG(INFO) << "Version: " << MESOS_VERSION;

  if (build::GIT_TAG.isSome()) {
    LOG(INFO) << "Git tag: " << build::GIT_TAG.get();
  }

  if (build::GIT_SHA.isSome()) {
    LOG(INFO) << "Git SHA: " << build::GIT_SHA.get();
  }

  allocator::AllocatorProcess* allocatorProcess =
    new allocator::HierarchicalDRFAllocatorProcess();
  allocator::Allocator* allocator =
    new allocator::Allocator(allocatorProcess);

  state::Storage* storage = NULL;

  if (strings::startsWith(flags.registry, "zk://")) {
    // TODO(benh):
    EXIT(1) << "ZooKeeper based registry unimplemented";
  } else if (flags.registry == "local") {
    storage = new state::LevelDBStorage(path::join(flags.work_dir, "registry"));
  } else {
    EXIT(1) << "'" << flags.registry << "' is not a supported"
            << " option for registry persistence";
  }

  CHECK_NOTNULL(storage);

  state::protobuf::State* state = new state::protobuf::State(storage);
  Registrar* registrar = new Registrar(state);

  Files files;

  MasterContender* contender;
  MasterDetector* detector;

  Try<MasterContender*> contender_ = MasterContender::create(zk);
  if (contender_.isError()) {
    EXIT(1) << "Failed to create a master contender: " << contender_.error();
  }
  contender = contender_.get();

  Try<MasterDetector*> detector_ = MasterDetector::create(zk);
  if (detector_.isError()) {
    EXIT(1) << "Failed to create a master detector: " << detector_.error();
  }
  detector = detector_.get();

  LOG(INFO) << "Starting Mesos master";

  Master* master = new Master(
      allocator, registrar, &files, contender, detector, flags);

  if (zk == "") {
    // It means we are using the standalone detector so we need to
    // appoint this Master as the leader.
    dynamic_cast<StandaloneMasterDetector*>(detector)->appoint(master->info());
  }

  process::spawn(master);

  process::wait(master->self());
  delete master;
  delete allocator;
  delete allocatorProcess;

  delete registrar;
  delete state;
  delete storage;

  delete contender;
  delete detector;

  return 0;
}
