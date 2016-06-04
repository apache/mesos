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

/**
 * This file defines the _mesos.so binary module used by the Mesos Python API.
 * This module contains private implementations of MesosSchedulerDriver and
 * MesosExecutorDriver as Python types that get called from the public module
 * called mesos (in <root>/src/python/src/mesos.py). This design was chosen
 * so that most of the API (e.g. the Scheduler and Executor interfaces) can
 * be written in Python, and only the parts that need to call into C++ are
 * in C++. Note that the mesos module also contains public classes called
 * MesosSchedulerDriver and MesosExecutorDriver. These call into the private
 * _mesos.MesosSchedulerDriverImpl and _mesos.MesosExecutorDriverImpl.
 */

// Python.h must be included before standard headers.
// See: http://docs.python.org/2/c-api/intro.html#include-files
#include <Python.h>

#include <iostream>

#include <mesos/scheduler.hpp>

#include "common.hpp"
#include "mesos_scheduler_driver_impl.hpp"
#include "proxy_scheduler.hpp"

using namespace mesos;
using namespace mesos::python;

using std::map;
using std::string;
using std::vector;


/**
 * The Python module object for mesos_pb2 (which contains the protobuf
 * classes generated for Python).
 */
PyObject* mesos::python::mesos_pb2 = nullptr;


namespace {

/**
 * Method list for our Python module.
 */
PyMethodDef MODULE_METHODS[] = {
  {nullptr, nullptr, 0, nullptr}        /* Sentinel */
};

} // namespace {


/**
 * Entry point called by Python to initialize our module.
 */
PyMODINIT_FUNC init_scheduler()
{
  // Ensure that the interpreter's threading support is enabled.
  PyEval_InitThreads();

  // Import the mesos_pb2 module (on which we depend for protobuf classes)
  mesos_pb2 = PyImport_ImportModule("mesos.interface.mesos_pb2");
  if (mesos_pb2 == nullptr)
    return;

  // Initialize our Python types.
  if (PyType_Ready(&MesosSchedulerDriverImplType) < 0)
    return;

  // Create the _mesos module and add our types to it.
  PyObject* module = Py_InitModule("_scheduler", MODULE_METHODS);
  Py_INCREF(&MesosSchedulerDriverImplType);
  PyModule_AddObject(module,
                     "MesosSchedulerDriverImpl",
                     (PyObject*) &MesosSchedulerDriverImplType);
}
