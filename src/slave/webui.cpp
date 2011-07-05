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

#include "state.hpp"
#include "webui.hpp"

#include "configurator/configuration.hpp"

#ifdef MESOS_WEBUI

#include <Python.h>

using process::PID;

using std::string;


extern "C" void init_slave();  // Initializer for the Python slave module


namespace mesos { namespace internal { namespace slave {

static PID<Slave> slave;
static string webuiPort;
static string logDir;
static string workDir;


void* runSlaveWebUI(void*)
{
  LOG(INFO) << "Web UI thread started";
  Py_Initialize();
  char* argv[4];
  argv[0] = const_cast<char*>("webui/master/webui.py");
  argv[1] = const_cast<char*>(webuiPort.c_str());
  argv[2] = const_cast<char*>(logDir.c_str());
  argv[3] = const_cast<char*>(workDir.c_str());
  PySys_SetArgv(4, argv);
  PyRun_SimpleString("import sys\n"
      "sys.path.append('webui/slave/swig')\n"
      "sys.path.append('webui/common')\n"
      "sys.path.append('webui/bottle-0.8.3')\n");
  init_slave();
  LOG(INFO) << "Loading webui/slave/webui.py";
  FILE *webui = fopen("webui/slave/webui.py", "r");
  PyRun_SimpleFile(webui, "webui/slave/webui.py");
  fclose(webui);
  Py_Finalize();
}


void startSlaveWebUI(const PID<Slave>& _slave, const Configuration& conf)
{
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

  LOG(INFO) << "Starting slave web UI on port " << webuiPort;

  slave = _slave;
  pthread_t thread;
  pthread_create(&thread, 0, runSlaveWebUI, NULL);
}


namespace state {

// From slave_state.hpp.
SlaveState* get_slave()
{
  return process::call(slave, &Slave::getState);
}

} // namespace state {

}}} // namespace mesos { namespace internal { namespace slave {


#endif // MESOS_WEBUI
