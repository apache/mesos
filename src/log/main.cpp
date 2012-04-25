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

#include <iostream>
#include <list>
#include <string>

#include <process/process.hpp>

#include "common/foreach.hpp"
#include "common/logging.hpp"
#include "common/utils.hpp"

#include "configurator/configurator.hpp"
#include "configurator/configuration.hpp"

#include "log/replica.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::log;

using std::cerr;
using std::cout;
using std::endl;
using std::string;


void usage(const char* argv0, const Configurator& configurator)
{
  cerr << "Usage: " << utils::os::basename(argv0) << " [...] path/to/log"
       << endl
       << endl
       << "Supported options:" << endl
       << configurator.getUsage();
}


int main(int argc, char** argv)
{
  Configurator configurator;
  Logging::registerOptions(&configurator);

  configurator.addOption<uint64_t>(
      "from",
      "Position from which to start reading in the log");

  configurator.addOption<uint64_t>(
      "to",
      "Position from which to stop reading in the log");

  configurator.addOption<bool>(
      "help",
      'h',
      "Prints this usage message");

  Configuration conf;
  try {
    conf = configurator.load(argc, argv);
  } catch (const ConfigurationException& e) {
    cerr << "Configuration error: " << e.what() << endl;
    usage(argv[0], configurator);
    exit(1);
  }

  if (argc < 2 || conf.contains("help")) {
    usage(argv[0], configurator);
    exit(1);
  }

  Logging::init(argv[0], conf);

  string path = argv[argc - 1];

  process::initialize(false);

  Replica replica(path);

  process::Future<uint64_t> begin = replica.beginning();
  process::Future<uint64_t> end = replica.ending();

  begin.await();
  end.await();

  CHECK(begin.isReady());
  CHECK(end.isReady());

  uint64_t from = conf.get<uint64_t>("from", begin.get());
  uint64_t to = conf.get<uint64_t>("to", end.get());

  cerr << endl << "Attempting to read the log from "
       << from << " to " << to << endl << endl;

  process::Future<std::list<Action> > actions = replica.read(from, to);

  actions.await();

  CHECK(!actions.isFailed()) << actions.failure();

  CHECK(actions.isReady());

  foreach (const Action& action, actions.get()) {
    cout << "----------------------------------------------" << endl;
    action.PrintDebugString();
  }

  return 0;
}
