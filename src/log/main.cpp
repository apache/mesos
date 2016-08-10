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

#include <string.h>

#include <iostream>
#include <string>

#include <process/owned.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>

#include "log/tool.hpp"
#include "log/tool/benchmark.hpp"
#include "log/tool/initialize.hpp"
#include "log/tool/read.hpp"
#include "log/tool/replica.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::log;

using namespace process;

using std::cerr;
using std::endl;
using std::string;

// All the registered tools.
static hashmap<string, Owned<tool::Tool>> tools;


static void add(const Owned<tool::Tool>& tool)
{
  tools[tool->name()] = tool;
}


static void usage(const char* argv0)
{
  cerr << "Usage: " << argv0 << " <command> [OPTIONS]" << endl
       << endl
       << "Available commands:" << endl
       << "    help" << endl;

  // Get a list of available tools.
  foreachkey (const string& name, tools) {
    cerr << "    " << name << endl;
  }
}


int main(int argc, char** argv)
{
  // Register log tools.
  add(Owned<tool::Tool>(new tool::Benchmark()));
  add(Owned<tool::Tool>(new tool::Initialize()));
  add(Owned<tool::Tool>(new tool::Read()));
  add(Owned<tool::Tool>(new tool::Replica()));

  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  if (!strcmp(argv[1], "help")) {
    if (argc == 2) {
      usage(argv[0]);
      return 0;
    }

    // 'mesos-log help command' => 'mesos-log command --help'
    argv[1] = argv[2];
    argv[2] = (char*) "--help";
  }

  string command = argv[1];

  if (!tools.contains(command)) {
    cerr << "Cannot find command '" << command << "'" << endl << endl;
    usage(argv[0]);
    return 1;
  }

  // Execute the command.
  Try<Nothing> execute = tools[command]->execute(argc, argv);
  if (execute.isError()) {
    cerr << execute.error() << endl;
    return 1;
  }

  return 0;
}
