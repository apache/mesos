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
#include <stout/flags.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/build.hpp"

#include "master/detector.hpp"

#include "logging/logging.hpp"

#include "slave/slave.hpp"

using namespace mesos::internal;
using namespace mesos::internal::slave;

using mesos::SlaveInfo;

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
            "  file://path/to/file (where file contains one of the above)");

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  Try<Nothing> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    EXIT(1);
  }

  if (help) {
    usage(argv[0], flags);
    EXIT(1);
  }

  if (master.isNone()) {
    EXIT(1) << "Missing required option --master";
  }

  // Initialize libprocess.
  if (ip.isSome()) {
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

  Try<Containerizer*> containerizer = Containerizer::create(flags, false);
  if (containerizer.isError()) {
    EXIT(1) << "Failed to create a containerizer: "
            << containerizer.error();
  }

  Try<MasterDetector*> detector = MasterDetector::create(master.get());
  if (detector.isError()) {
    EXIT(1) << "Failed to create a master detector: " << detector.error();
  }

  LOG(INFO) << "Starting Mesos slave";

  Files files;
  Slave* slave = new Slave(flags, detector.get(), containerizer.get(), &files);
  process::spawn(slave);

  process::wait(slave->self());
  delete slave;

  delete detector.get();

  delete containerizer.get();

  return 0;
}
