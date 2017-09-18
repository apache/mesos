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

#include <iostream>
#include <string>

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include "local/flags.hpp"
#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "logging/logging.hpp"

#include "version/version.hpp"

using namespace mesos::internal;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using std::cerr;
using std::cout;
using std::endl;
using std::string;


class Flags : public virtual local::Flags
{
public:
  Flags()
  {
    add(&Flags::port, "port", "Port to listen on", 5050);
    add(&Flags::ip, "ip", "IP address to listen on");
  }

  // The following flags are executable specific (e.g., since we only
  // have one instance of libprocess per execution, we only want to
  // advertise the port and ip option once, here).

  uint16_t port;
  Option<string> ip;
};


int main(int argc, char **argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // TODO(benh): Inherit from both slave::Flags and master::Flags in
  // order to pass those flags on to the master. Alternatively, add a
  // way to load flags and ignore unknowns in order to load
  // master::flags, then slave::Flags, then local::Flags.
  Flags flags;

  flags.setUsageMessage(
      "Usage: " + Path(argv[0]).basename() + " [...]\n\n" +
      "Launches an in-memory cluster within a single process.");

  // Load flags from environment and command line but allow unknown
  // flags since we might have some master/slave flags as well.
  Try<flags::Warnings> load = flags.load("MESOS_", argc, argv, true);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  logging::initialize(argv[0], false, flags);

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  // Initialize libprocess.
  os::setenv("LIBPROCESS_PORT", stringify(flags.port));

  if (flags.ip.isSome()) {
    os::setenv("LIBPROCESS_IP", flags.ip.get());
  }

  process::initialize("master");

  spawn(new VersionProcess(), true);

  process::wait(local::launch(flags));

  return EXIT_SUCCESS;
}
