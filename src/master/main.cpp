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

#include <set>

#include <mesos/mesos.hpp>

#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/check.hpp>
#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "authorizer/authorizer.hpp"

#include "common/build.hpp"
#include "common/protobuf_utils.hpp"

#include "hook/manager.hpp"

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

#include "module/manager.hpp"

#include "state/in_memory.hpp"
#include "state/log.hpp"
#include "state/protobuf.hpp"
#include "state/storage.hpp"


#include "zookeeper/detector.hpp"

using namespace mesos;
using namespace mesos::log;
using namespace mesos::master;
using namespace mesos::state;
using namespace zookeeper;

using mesos::MasterInfo;
using mesos::modules::ModuleManager;

using process::Owned;
using process::UPID;

using std::cerr;
using std::cout;
using std::endl;
using std::set;
using std::string;


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << os::basename(argv0).get() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


void version()
{
  cout << "mesos" << " " << MESOS_VERSION << endl;
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

  Option<string> zk;
  flags.add(&zk,
            "zk",
            "ZooKeeper URL (used for leader election amongst masters)\n"
            "May be one of:\n"
            "  zk://host1:port1,host2:port2,.../path\n"
            "  zk://username:password@host1:port1,host2:port2,.../path\n"
            "  file:///path/to/file (where file contains one of the above)");

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

  if (flags.version) {
    version();
    exit(0);
  }

  if (help) {
    usage(argv[0], flags);
    exit(1);
  }

  // Initialize modules. Note that since other subsystems may depend
  // upon modules, we should initialize modules before anything else.
  if (flags.modules.isSome()) {
    Try<Nothing> result = ModuleManager::load(flags.modules.get());
    if (result.isError()) {
      EXIT(1) << "Error loading modules: " << result.error();
    }
  }

  // Initialize hooks.
  if (flags.hooks.isSome()) {
    Try<Nothing> result = HookManager::initialize(flags.hooks.get());
    if (result.isError()) {
      EXIT(1) << "Error installing hooks: " << result.error();
    }
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

  Storage* storage = NULL;
  Log* log = NULL;

  if (flags.registry == "in_memory") {
    if (flags.registry_strict) {
      EXIT(1) << "Cannot use '--registry_strict' when using in-memory storage"
              << " based registry";
    }
    storage = new InMemoryStorage();
  } else if (flags.registry == "replicated_log" ||
             flags.registry == "log_storage") {
    // TODO(bmahler): "log_storage" is present for backwards
    // compatibility, can be removed before 0.19.0.
    if (flags.work_dir.isNone()) {
      EXIT(1) << "--work_dir needed for replicated log based registry";
    }

    Try<Nothing> mkdir = os::mkdir(flags.work_dir.get());
    if (mkdir.isError()) {
      EXIT(1) << "Failed to create work directory '" << flags.work_dir.get()
              << "': " << mkdir.error();
    }

    if (zk.isSome()) {
      // Use replicated log with ZooKeeper.
      if (flags.quorum.isNone()) {
        EXIT(1) << "Need to specify --quorum for replicated log based"
                << " registry when using ZooKeeper";
      }

      string zk_;
      if (strings::startsWith(zk.get(), "file://")) {
        const string& path = zk.get().substr(7);
        const Try<string> read = os::read(path);
        if (read.isError()) {
          EXIT(1) << "Failed to read from file at '" + path + "': "
                  << read.error();
        }
        zk_ = read.get();
      } else {
        zk_ = zk.get();
      }

      Try<zookeeper::URL> url = zookeeper::URL::parse(zk_);
      if (url.isError()) {
        EXIT(1) << "Error parsing ZooKeeper URL: " << url.error();
      }

      log = new Log(
          flags.quorum.get(),
          path::join(flags.work_dir.get(), "replicated_log"),
          url.get().servers,
          flags.zk_session_timeout,
          path::join(url.get().path, "log_replicas"),
          url.get().authentication,
          flags.log_auto_initialize);
    } else {
      // Use replicated log without ZooKeeper.
      log = new Log(
          1,
          path::join(flags.work_dir.get(), "replicated_log"),
          set<UPID>(),
          flags.log_auto_initialize);
    }
    storage = new LogStorage(log);
  } else {
    EXIT(1) << "'" << flags.registry << "' is not a supported"
            << " option for registry persistence";
  }

  CHECK_NOTNULL(storage);

  mesos::state::protobuf::State* state =
    new mesos::state::protobuf::State(storage);
  Registrar* registrar = new Registrar(flags, state);
  Repairer* repairer = new Repairer();

  Files files;

  MasterContender* contender;
  MasterDetector* detector;

  // TODO(vinod): 'MasterContender::create()' should take
  // Option<string>.
  Try<MasterContender*> contender_ = MasterContender::create(zk.get(""));
  if (contender_.isError()) {
    EXIT(1) << "Failed to create a master contender: " << contender_.error();
  }
  contender = contender_.get();

  // TODO(vinod): 'MasterDetector::create()' should take
  // Option<string>.
  Try<MasterDetector*> detector_ = MasterDetector::create(zk.get(""));
  if (detector_.isError()) {
    EXIT(1) << "Failed to create a master detector: " << detector_.error();
  }
  detector = detector_.get();

  Option<Authorizer*> authorizer = None();
  if (flags.acls.isSome()) {
    Try<Owned<Authorizer> > authorizer_ = Authorizer::create(flags.acls.get());
    if (authorizer_.isError()) {
      EXIT(1) << "Failed to initialize the authorizer: "
              << authorizer_.error() << " (see --acls flag)";
    }
    Owned<Authorizer> authorizer__ = authorizer_.get();
    authorizer = authorizer__.release();
  }

  LOG(INFO) << "Starting Mesos master";

  Master* master =
    new Master(
      allocator,
      registrar,
      repairer,
      &files,
      contender,
      detector,
      authorizer,
      flags);

  if (zk.isNone()) {
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
  delete repairer;
  delete state;
  delete storage;
  delete log;

  delete contender;
  delete detector;

  if (authorizer.isSome()) {
    delete authorizer.get();
  }

  return 0;
}
