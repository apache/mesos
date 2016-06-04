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

// Python.h must be included before standard headers.
// See: http://docs.python.org/2/c-api/intro.html#include-files
#include <Python.h>

#include <iostream>

#include "common.hpp"
#include "mesos_executor_driver_impl.hpp"
#include "proxy_executor.hpp"

using namespace mesos;

using std::cerr;
using std::endl;
using std::map;
using std::string;
using std::vector;

namespace mesos {
namespace python {

void ProxyExecutor::registered(ExecutorDriver* driver,
                               const ExecutorInfo& executorInfo,
                               const FrameworkInfo& frameworkInfo,
                               const SlaveInfo& slaveInfo)
{
  InterpreterLock lock;

  PyObject* executorInfoObj = nullptr;
  PyObject* frameworkInfoObj = nullptr;
  PyObject* slaveInfoObj = nullptr;
  PyObject* res = nullptr;

  executorInfoObj = createPythonProtobuf(executorInfo, "ExecutorInfo");
  frameworkInfoObj = createPythonProtobuf(frameworkInfo, "FrameworkInfo");
  slaveInfoObj = createPythonProtobuf(slaveInfo, "SlaveInfo");

  if (executorInfoObj == nullptr ||
      frameworkInfoObj == nullptr ||
      slaveInfoObj == nullptr) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "registered",
                            (char*) "OOOO",
                            impl,
                            executorInfoObj,
                            frameworkInfoObj,
                            slaveInfoObj);
  if (res == nullptr) {
    cerr << "Failed to call executor registered" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(executorInfoObj);
  Py_XDECREF(frameworkInfoObj);
  Py_XDECREF(slaveInfoObj);
  Py_XDECREF(res);
}


void ProxyExecutor::reregistered(ExecutorDriver* driver,
                                 const SlaveInfo& slaveInfo)
{
  InterpreterLock lock;

  PyObject* slaveInfoObj = nullptr;
  PyObject* res = nullptr;

  slaveInfoObj = createPythonProtobuf(slaveInfo, "SlaveInfo");

  if (slaveInfoObj == nullptr) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "reregistered",
                            (char*) "OO",
                            impl,
                            slaveInfoObj);
  if (res == nullptr) {
    cerr << "Failed to call executor re-registered" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(slaveInfoObj);
  Py_XDECREF(res);
}


void ProxyExecutor::disconnected(ExecutorDriver* driver)
{
  InterpreterLock lock;
  PyObject* res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "disconnected",
                            (char*) "O",
                            impl);
  if (res == nullptr) {
    cerr << "Failed to call executor's disconnected" << endl;
    goto cleanup;
  }
cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(res);
}


void ProxyExecutor::launchTask(ExecutorDriver* driver,
                               const TaskInfo& task)
{
  InterpreterLock lock;

  PyObject* taskObj = nullptr;
  PyObject* res = nullptr;

  taskObj = createPythonProtobuf(task, "TaskInfo");
  if (taskObj == nullptr) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "launchTask",
                            (char*) "OO",
                            impl,
                            taskObj);
  if (res == nullptr) {
    cerr << "Failed to call executor's launchTask" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(taskObj);
  Py_XDECREF(res);
}


void ProxyExecutor::killTask(ExecutorDriver* driver,
                             const TaskID& taskId)
{
  InterpreterLock lock;

  PyObject* taskIdObj = nullptr;
  PyObject* res = nullptr;

  taskIdObj = createPythonProtobuf(taskId, "TaskID");
  if (taskIdObj == nullptr) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "killTask",
                            (char*) "OO",
                            impl,
                            taskIdObj);
  if (res == nullptr) {
    cerr << "Failed to call executor's killTask" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(taskIdObj);
  Py_XDECREF(res);
}


void ProxyExecutor::frameworkMessage(ExecutorDriver* driver,
                                     const string& data)
{
  InterpreterLock lock;

  PyObject* res = nullptr;

  res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "frameworkMessage",
                            (char*) "Os#",
                            impl,
                            data.data(),
                            data.length());
  if (res == nullptr) {
    cerr << "Failed to call executor's frameworkMessage" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(res);
}


void ProxyExecutor::shutdown(ExecutorDriver* driver)
{
  InterpreterLock lock;
  PyObject* res = PyObject_CallMethod(impl->pythonExecutor,
                                      (char*) "shutdown",
                                      (char*) "O",
                                      impl);
  if (res == nullptr) {
    cerr << "Failed to call executor's shutdown" << endl;
    goto cleanup;
  }
cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(res);
}


void ProxyExecutor::error(ExecutorDriver* driver, const string& message)
{
  InterpreterLock lock;
  PyObject* res = PyObject_CallMethod(impl->pythonExecutor,
                                      (char*) "error",
                                      (char*) "Os#",
                                      impl,
                                      message.data(),
                                      message.length());
  if (res == nullptr) {
    cerr << "Failed to call executor's error" << endl;
    goto cleanup;
  }
cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    // No need for driver.stop(); it should stop itself.
  }
  Py_XDECREF(res);
}

} // namespace python {
} // namespace mesos {
