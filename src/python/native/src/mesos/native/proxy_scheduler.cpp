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

#include "proxy_scheduler.hpp"
#include "module.hpp"
#include "mesos_scheduler_driver_impl.hpp"

using namespace mesos;

using std::cerr;
using std::endl;
using std::string;
using std::vector;
using std::map;

namespace mesos {
namespace python {

void ProxyScheduler::registered(SchedulerDriver* driver,
                                const FrameworkID& frameworkId,
                                const MasterInfo& masterInfo)
{
  InterpreterLock lock;

  PyObject* fid = NULL;
  PyObject* minfo = NULL;
  PyObject* res = NULL;

  fid = createPythonProtobuf(frameworkId, "FrameworkID");
  if (fid == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  minfo = createPythonProtobuf(masterInfo, "MasterInfo");
  if (minfo == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "registered",
                            (char*) "OOO",
                            impl,
                            fid,
                            minfo);
  if (res == NULL) {
    cerr << "Failed to call scheduler's registered" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(fid);
  Py_XDECREF(minfo);
  Py_XDECREF(res);
}


void ProxyScheduler::reregistered(SchedulerDriver* driver,
                                  const MasterInfo& masterInfo)
{
  InterpreterLock lock;

  PyObject* minfo = NULL;
  PyObject* res = NULL;

  minfo = createPythonProtobuf(masterInfo, "MasterInfo");
  if (minfo == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "reregistered",
                            (char*) "OO",
                            impl,
                            minfo);
  if (res == NULL) {
    cerr << "Failed to call scheduler's reregistered" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(minfo);
  Py_XDECREF(res);
}


void ProxyScheduler::disconnected(SchedulerDriver* driver)
{
  InterpreterLock lock;

  PyObject* res = NULL;

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "disconnected",
                            (char*) "O",
                            impl);
  if (res == NULL) {
    cerr << "Failed to call scheduler's disconnected" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(res);
}


void ProxyScheduler::resourceOffers(SchedulerDriver* driver,
                                    const vector<Offer>& offers)
{
  InterpreterLock lock;

  PyObject* list = NULL;
  PyObject* res = NULL;

  list = PyList_New(offers.size());
  if (list == NULL) {
    goto cleanup;
  }
  for (size_t i = 0; i < offers.size(); i++) {
    PyObject* offer = createPythonProtobuf(offers[i], "Offer");
    if (offer == NULL) {
      goto cleanup;
    }
    PyList_SetItem(list, i, offer); // Steals the reference to offer.
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "resourceOffers",
                            (char*) "OO",
                            impl,
                            list);

  if (res == NULL) {
    cerr << "Failed to call scheduler's resourceOffer" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(list);
  Py_XDECREF(res);
}


void ProxyScheduler::offerRescinded(SchedulerDriver* driver,
                                    const OfferID& offerId)
{
  InterpreterLock lock;

  PyObject* oid = NULL;
  PyObject* res = NULL;

  oid = createPythonProtobuf(offerId, "OfferID");
  if (oid == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "offerRescinded",
                            (char*) "OO",
                            impl,
                            oid);
  if (res == NULL) {
    cerr << "Failed to call scheduler's offerRescinded" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(oid);
  Py_XDECREF(res);
}


void ProxyScheduler::statusUpdate(SchedulerDriver* driver,
                                  const TaskStatus& status)
{
  InterpreterLock lock;

  PyObject* stat = NULL;
  PyObject* res = NULL;

  stat = createPythonProtobuf(status, "TaskStatus");
  if (stat == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "statusUpdate",
                            (char*) "OO",
                            impl,
                            stat);
  if (res == NULL) {
    cerr << "Failed to call scheduler's statusUpdate" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(stat);
  Py_XDECREF(res);
}


void ProxyScheduler::frameworkMessage(SchedulerDriver* driver,
                                      const ExecutorID& executorId,
                                      const SlaveID& slaveId,
                                      const string& data)
{
  InterpreterLock lock;

  PyObject* eid = NULL;
  PyObject* sid = NULL;
  PyObject* res = NULL;

  eid = createPythonProtobuf(executorId, "ExecutorID");
  if (eid == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  sid = createPythonProtobuf(slaveId, "SlaveID");
  if (sid == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "frameworkMessage",
                            (char*) "OOOs#",
                            impl,
                            eid,
                            sid,
                            data.data(),
                            data.length());
  if (res == NULL) {
    cerr << "Failed to call scheduler's frameworkMessage" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(eid);
  Py_XDECREF(sid);
  Py_XDECREF(res);
}


void ProxyScheduler::slaveLost(SchedulerDriver* driver, const SlaveID& slaveId)
{
  InterpreterLock lock;

  PyObject* sid = NULL;
  PyObject* res = NULL;

  sid = createPythonProtobuf(slaveId, "SlaveID");
  if (sid == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "slaveLost",
                            (char*) "OO",
                            impl,
                            sid);
  if (res == NULL) {
    cerr << "Failed to call scheduler's slaveLost" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(sid);
  Py_XDECREF(res);
}


void ProxyScheduler::executorLost(SchedulerDriver* driver,
                                  const ExecutorID& executorId,
                                  const SlaveID& slaveId,
                                  int status)
{
  InterpreterLock lock;

  PyObject* executorIdObj = NULL;
  PyObject* slaveIdObj = NULL;
  PyObject* res = NULL;

  executorIdObj = createPythonProtobuf(executorId, "ExecutorID");
  slaveIdObj = createPythonProtobuf(slaveId, "SlaveID");

  if (executorIdObj == NULL || slaveIdObj == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception.
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "executorLost",
                            (char*) "OOOi",
                            impl,
                            executorIdObj,
                            slaveIdObj,
                            status);
  if (res == NULL) {
    cerr << "Failed to call scheduler's executorLost" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->abort();
  }
  Py_XDECREF(executorIdObj);
  Py_XDECREF(slaveIdObj);
  Py_XDECREF(res);
}


void ProxyScheduler::error(SchedulerDriver* driver, const string& message)
{
  InterpreterLock lock;
  PyObject* res = PyObject_CallMethod(impl->pythonScheduler,
                                      (char*) "error",
                                      (char*) "Os#",
                                      impl,
                                      message.data(),
                                      message.length());
  if (res == NULL) {
    cerr << "Failed to call scheduler's error" << endl;
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
