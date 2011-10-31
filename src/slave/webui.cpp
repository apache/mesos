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

#include <pthread.h>

#include <sstream>
#include <string>

#include <process/dispatch.hpp>

#include "slave/webui.hpp"

#include "common/utils.hpp"

#include "configurator/configuration.hpp"

#ifdef MESOS_WEBUI

#include <Python.h>


namespace mesos {
namespace internal {
namespace slave {
namespace webui {

static std::string slavePort;
static std::string webuiPort;
static std::string logDir;
static std::string workDir;


void* run(void*)
{
  LOG(INFO) << "Slave web server thread started";
  Py_Initialize();
  char* argv[5];
  argv[0] = const_cast<char*>("webui/slave/webui.py");
  argv[1] = const_cast<char*>(slavePort.c_str());
  argv[2] = const_cast<char*>(webuiPort.c_str());
  argv[3] = const_cast<char*>(logDir.c_str());
  argv[4] = const_cast<char*>(workDir.c_str());
  PySys_SetArgv(5, argv);
  PyRun_SimpleString(
      "import sys\n"
      "sys.path.append('webui/common')\n"
      "sys.path.append('webui/bottle-0.8.3')\n");
  LOG(INFO) << "Loading webui/slave/webui.py";
  FILE* file = fopen("webui/slave/webui.py", "r");
  PyRun_SimpleFile(file, "webui/slave/webui.py");
  fclose(file);
  Py_Finalize();
}


void start(const process::PID<Slave>& slave, const Configuration& conf)
{
  slavePort = utils::stringify(slave.port);

  // TODO(*): See the note in master/webui.cpp about having to
  // determine default values. These should be set by now and can just
  // be used! For example, what happens when the slave code changes
  // their default location for the work directory, it might not get
  // changed here!
  webuiPort = conf.get("webui_port", "8081");
  logDir = conf.get("log_dir", FLAGS_log_dir);
  if (conf.contains("work_dir")) {
    workDir = conf.get("work_dir", "");
  } else if (conf.contains("home")) {
    workDir = conf.get("home", "") + "/work";
  } else {
    workDir = "work";
  }

  CHECK(workDir != "");

  LOG(INFO) << "Starting slave web server on port " << webuiPort;

  pthread_t thread;
  if (pthread_create(&thread, 0, run, NULL) != 0) {
    LOG(FATAL) << "Failed to create slave web server thread";
  }
}

} // namespace webui {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // MESOS_WEBUI
