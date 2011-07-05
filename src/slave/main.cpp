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

#include <libgen.h>

#include "common/build.hpp"
#include "common/logging.hpp"

#include "configurator/configurator.hpp"

#include "detector/detector.hpp"

#include "isolation_module_factory.hpp"
#include "slave.hpp"
#include "webui.hpp"

using namespace mesos::internal;
using namespace mesos::internal::slave;

using std::cerr;
using std::endl;
using std::string;


void usage(const char *programName, const Configurator& configurator)
{
  cerr << "Usage: " << programName
       << " --url=MASTER_URL [--cpus=NUM] [--mem=BYTES] [...]" << endl
       << endl
       << "MASTER_URL may be one of:" << endl
       << "  mesos://id@host:port" << endl
       << "  zoo://host1:port1,host2:port2,..." << endl
       << "  zoofile://file where file contains a host:port pair per line"
       << endl
       << endl
       << "Supported options:" << endl
       << configurator.getUsage();
}


int main(int argc, char **argv)
{
  Configurator configurator;
  Logging::registerOptions(&configurator);
  Slave::registerOptions(&configurator);
  configurator.addOption<int>("port", 'p', "Port to listen on", 0);
  configurator.addOption<string>("ip", "IP address to listen on");
  configurator.addOption<string>("url", 'u', "Master URL");
  configurator.addOption<string>("isolation", 'i', "Isolation module name", "process");
#ifdef MESOS_WEBUI
  configurator.addOption<int>("webui_port", 'w', "Web UI port", 8081);
#endif

  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0], configurator);
    exit(1);
  }

  Configuration conf;
  try {
    conf = configurator.load(argc, argv, true);
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

  if (!conf.contains("url")) {
    cerr << "Master URL argument (--url) required." << endl;
    exit(1);
  }
  string url = conf["url"];

  string isolation = conf["isolation"];
  LOG(INFO) << "Creating \"" << isolation << "\" isolation module";
  IsolationModule* isolationModule = IsolationModule::create(isolation);

  if (isolationModule == NULL) {
    cerr << "Unrecognized isolation type: " << isolation << endl;
    exit(1);
  }

  LOG(INFO) << "Build: " << build::DATE << " by " << build::USER;
  LOG(INFO) << "Starting Mesos slave";

  if (chdir(dirname(argv[0])) != 0) {
    fatalerror("Could not chdir into %s", dirname(argv[0]));
  }

  Slave* slave = new Slave(conf, false, isolationModule);
  process::spawn(slave);

  MasterDetector* detector =
    MasterDetector::create(url, slave->self(), false, Logging::isQuiet(conf));

#ifdef MESOS_WEBUI
  startSlaveWebUI(slave->self(), conf);
#endif

  process::wait(slave->self());
  delete slave;

  MasterDetector::destroy(detector);
  IsolationModule::destroy(isolationModule);

  return 0;
}
