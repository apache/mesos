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

#include <libgen.h> // For chdir.

#include "common/build.hpp"
#include "common/fatal.hpp"
#include "common/logging.hpp"
#include "common/try.hpp"

#include "configurator/configurator.hpp"

#include "detector/detector.hpp"

#include "master/allocator.hpp"
#include "master/simple_allocator.hpp"
#include "master/master.hpp"
#include "master/webui.hpp"

using namespace mesos::internal;
using namespace mesos::internal::master;

using std::cerr;
using std::endl;
using std::string;


void usage(const char* programName, const Configurator& configurator)
{
  cerr << "Usage: " << programName << " [...]" << endl
       << endl
       << endl
       << "Supported options:" << endl
       << configurator.getUsage();
}


int main(int argc, char **argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  Configurator configurator;
  Logging::registerOptions(&configurator);
  Master::registerOptions(&configurator);

  // The following options are executable specific (e.g., since we
  // only have one instance of libprocess per execution, we only want
  // to advertise the port and ip option once, here).
  configurator.addOption<int>("port", 'p', "Port to listen on", 5050);
  configurator.addOption<string>("ip", "IP address to listen on");
#ifdef MESOS_WEBUI
  configurator.addOption<int>("webui_port", "Web UI port", 8080);
#endif
  configurator.addOption<string>(
      "zk",
      "ZooKeeper URL (used for leader election amongst masters)\n"
      "May be one of:\n"
      "  zk://host1:port1,host2:port2,.../path\n"
      "  zk://username:password@host1:port1,host2:port2,.../path\n"
      "  file://path/to/file (where file contains one of the above)");

  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0], configurator);
    exit(1);
  }

  Configuration conf;
  try {
    conf = configurator.load(argc, argv);
  } catch (ConfigurationException& e) {
    cerr << "Configuration error: " << e.what() << endl;
    exit(1);
  }

  Logging::init(argv[0], conf);

  if (conf.contains("port")) {
    setenv("LIBPROCESS_PORT", conf["port"].c_str(), 1);
  }

  if (conf.contains("ip")) {
    setenv("LIBPROCESS_IP", conf["ip"].c_str(), 1);
  }

  // Initialize libprocess library (but not glog, done above).
  process::initialize(false);

  string zk = conf.get("zk", "");

  LOG(INFO) << "Build: " << build::DATE << " by " << build::USER;
  LOG(INFO) << "Starting Mesos master";

  if (chdir(dirname(argv[0])) != 0) {
    fatalerror("Could not chdir into %s", dirname(argv[0]));
  }

  Allocator* allocator = new SimpleAllocator();

  Master* master = new Master(allocator, conf);
  process::spawn(master);

  Try<MasterDetector*> detector =
    MasterDetector::create(zk, master->self(), true, Logging::isQuiet(conf));

  CHECK(detector.isSome())
    << "Failed to create a master detector: " << detector.error();

#ifdef MESOS_WEBUI
  webui::start(master->self(), conf);
#endif

  process::wait(master->self());
  delete master;
  delete allocator;

  MasterDetector::destroy(detector.get());

  return 0;
}
