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

#include <string>

#include "common.hpp"
#include "mesos_scheduler_driver_impl.hpp"
#include "proxy_scheduler.hpp"

using namespace mesos;
using namespace mesos::python;

using std::cerr;
using std::endl;
using std::string;
using std::vector;
using std::map;

namespace mesos {
namespace python {

/**
 * Python type object for MesosSchedulerDriverImpl.
 */
PyTypeObject MesosSchedulerDriverImplType = {
  PyObject_HEAD_INIT(nullptr)
  0,                                                /* ob_size */
  "_mesos.MesosSchedulerDriverImpl",                /* tp_name */
  sizeof(MesosSchedulerDriverImpl),                 /* tp_basicsize */
  0,                                                /* tp_itemsize */
  (destructor) MesosSchedulerDriverImpl_dealloc,    /* tp_dealloc */
  0,                                                /* tp_print */
  0,                                                /* tp_getattr */
  0,                                                /* tp_setattr */
  0,                                                /* tp_compare */
  0,                                                /* tp_repr */
  0,                                                /* tp_as_number */
  0,                                                /* tp_as_sequence */
  0,                                                /* tp_as_mapping */
  0,                                                /* tp_hash */
  0,                                                /* tp_call */
  0,                                                /* tp_str */
  0,                                                /* tp_getattro */
  0,                                                /* tp_setattro */
  0,                                                /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,          /* tp_flags */
  "Private MesosSchedulerDriver implementation",    /* tp_doc */
  (traverseproc) MesosSchedulerDriverImpl_traverse, /* tp_traverse */
  (inquiry) MesosSchedulerDriverImpl_clear,         /* tp_clear */
  0,                                                /* tp_richcompare */
  0,                                                /* tp_weaklistoffset */
  0,                                                /* tp_iter */
  0,                                                /* tp_iternext */
  MesosSchedulerDriverImpl_methods,                 /* tp_methods */
  0,                                                /* tp_members */
  0,                                                /* tp_getset */
  0,                                                /* tp_base */
  0,                                                /* tp_dict */
  0,                                                /* tp_descr_get */
  0,                                                /* tp_descr_set */
  0,                                                /* tp_dictoffset */
  (initproc) MesosSchedulerDriverImpl_init,         /* tp_init */
  0,                                                /* tp_alloc */
  MesosSchedulerDriverImpl_new,                     /* tp_new */
};


/**
 * List of Python methods in MesosSchedulerDriverImpl.
 */
PyMethodDef MesosSchedulerDriverImpl_methods[] = {
  { "start",
    (PyCFunction) MesosSchedulerDriverImpl_start,
    METH_NOARGS,
    "Start the driver to connect to Mesos"
  },
  { "stop",
    (PyCFunction) MesosSchedulerDriverImpl_stop,
    METH_VARARGS,
    "Stop the driver, disconnecting from Mesos"
  },
  { "abort",
    (PyCFunction) MesosSchedulerDriverImpl_abort,
    METH_NOARGS,
    "Abort the driver, disabling calls from and to the driver"
  },
  { "join",
    (PyCFunction) MesosSchedulerDriverImpl_join,
    METH_NOARGS,
    "Wait for a running driver to disconnect from Mesos"
  },
  { "run",
    (PyCFunction) MesosSchedulerDriverImpl_run,
    METH_NOARGS,
    "Start a driver and run it, returning when it disconnects from Mesos"
  },
  { "requestResources",
    (PyCFunction) MesosSchedulerDriverImpl_requestResources,
    METH_VARARGS,
    "Request resources from the Mesos allocator"
  },
  { "launchTasks",
    (PyCFunction) MesosSchedulerDriverImpl_launchTasks,
    METH_VARARGS,
    "Reply to a Mesos offer with a list of tasks"
  },
  { "killTask",
    (PyCFunction) MesosSchedulerDriverImpl_killTask,
    METH_VARARGS,
    "Kill the task with the given ID"
  },
  { "acceptOffers",
    (PyCFunction) MesosSchedulerDriverImpl_acceptOffers,
    METH_VARARGS,
    "Reply to a Mesos offer with a list of offer operations"
  },
  { "declineOffer",
    (PyCFunction) MesosSchedulerDriverImpl_declineOffer,
    METH_VARARGS,
    "Decline a Mesos offer"
  },
  { "reviveOffers",
    (PyCFunction) MesosSchedulerDriverImpl_reviveOffers,
    METH_NOARGS,
    "Remove all filters and ask Mesos for new offers"
  },
  { "suppressOffers",
    (PyCFunction) MesosSchedulerDriverImpl_suppressOffers,
    METH_NOARGS,
    "Set suppressed attribute as true for the Framework"
  },
  { "acknowledgeStatusUpdate",
    (PyCFunction) MesosSchedulerDriverImpl_acknowledgeStatusUpdate,
    METH_VARARGS,
    "Acknowledge a status update"
  },
  { "sendFrameworkMessage",
    (PyCFunction) MesosSchedulerDriverImpl_sendFrameworkMessage,
    METH_VARARGS,
    "Send a FrameworkMessage to an agent"
  },
  { "reconcileTasks",
    (PyCFunction) MesosSchedulerDriverImpl_reconcileTasks,
    METH_VARARGS,
    "Master sends status updates if task status is different from expected"
  },
  { nullptr }  /* Sentinel */
};


/**
 * Create, but don't initialize, a new MesosSchedulerDriverImpl
 * (called by Python before init method).
 */
PyObject* MesosSchedulerDriverImpl_new(PyTypeObject* type,
                                       PyObject* args,
                                       PyObject* kwds)
{
  MesosSchedulerDriverImpl* self;
  self = (MesosSchedulerDriverImpl*) type->tp_alloc(type, 0);
  if (self != nullptr) {
    self->driver = nullptr;
    self->proxyScheduler = nullptr;
    self->pythonScheduler = nullptr;
  }
  return (PyObject*) self;
}


/**
 * Initialize a MesosSchedulerDriverImpl with constructor arguments.
 */
int MesosSchedulerDriverImpl_init(MesosSchedulerDriverImpl* self,
                                  PyObject* args,
                                  PyObject* kwds)
{
  // Note: We use an integer for 'implicitAcknoweldgements' because
  // it is the recommended way to pass booleans through CPython.
  PyObject* schedulerObj = nullptr;
  PyObject* frameworkObj = nullptr;
  const char* master;
  int implicitAcknowledgements = 1; // Enabled by default.
  PyObject* credentialObj = nullptr;

  if (!PyArg_ParseTuple(
      args,
      "OOs|iO",
      &schedulerObj,
      &frameworkObj,
      &master,
      &implicitAcknowledgements,
      &credentialObj)) {
    return -1;
  }

  if (schedulerObj != nullptr) {
    PyObject* tmp = self->pythonScheduler;
    Py_INCREF(schedulerObj);
    self->pythonScheduler = schedulerObj;
    Py_XDECREF(tmp);
  }

  FrameworkInfo framework;
  if (frameworkObj != nullptr) {
    if (!readPythonProtobuf(frameworkObj, &framework)) {
      PyErr_Format(PyExc_Exception,
                   "Could not deserialize Python FrameworkInfo");
      return -1;
    }
  }

  Credential credential;
  if (credentialObj != nullptr) {
    if (!readPythonProtobuf(credentialObj, &credential)) {
      PyErr_Format(PyExc_Exception, "Could not deserialize Python Credential");
      return -1;
    }
  }


  if (self->driver != nullptr) {
    delete self->driver;
    self->driver = nullptr;
  }

  if (self->proxyScheduler != nullptr) {
    delete self->proxyScheduler;
    self->proxyScheduler = nullptr;
  }

  self->proxyScheduler = new ProxyScheduler(self);

  if (credentialObj != nullptr) {
    self->driver = new MesosSchedulerDriver(
        self->proxyScheduler,
        framework,
        master,
        implicitAcknowledgements != 0,
        credential);
  } else {
    self->driver = new MesosSchedulerDriver(
        self->proxyScheduler,
        framework,
        master,
        implicitAcknowledgements != 0);
  }

  return 0;
}


/**
 * Free a MesosSchedulerDriverImpl.
 */
void MesosSchedulerDriverImpl_dealloc(MesosSchedulerDriverImpl* self)
{
  if (self->driver != nullptr) {
    // We need to wrap the driver destructor in an "allow threads"
    // macro since the MesosSchedulerDriver destructor waits for the
    // SchedulerProcess to terminate and there might be a thread that
    // is trying to acquire the GIL to call through the
    // ProxyScheduler. It will only be after this thread executes that
    // the SchedulerProcess might actually get a terminate.
    Py_BEGIN_ALLOW_THREADS
    delete self->driver;
    Py_END_ALLOW_THREADS
    self->driver = nullptr;
  }

  if (self->proxyScheduler != nullptr) {
    delete self->proxyScheduler;
    self->proxyScheduler = nullptr;
  }

  MesosSchedulerDriverImpl_clear(self);
  self->ob_type->tp_free((PyObject*) self);
}


/**
 * Traverse fields of a MesosSchedulerDriverImpl on a cyclic GC search.
 * See http://docs.python.org/extending/newtypes.html.
 */
int MesosSchedulerDriverImpl_traverse(MesosSchedulerDriverImpl* self,
                                      visitproc visit,
                                      void* arg)
{
  Py_VISIT(self->pythonScheduler);
  return 0;
}


/**
 * Clear fields of a MesosSchedulerDriverImpl that can participate in
 * GC cycles. See http://docs.python.org/extending/newtypes.html.
 */
int MesosSchedulerDriverImpl_clear(MesosSchedulerDriverImpl* self)
{
  Py_CLEAR(self->pythonScheduler);
  return 0;
}


PyObject* MesosSchedulerDriverImpl_start(MesosSchedulerDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status = self->driver->start();
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_stop(MesosSchedulerDriverImpl* self,
                                        PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  bool failover = false; // Should match default in mesos.py.

  if (!PyArg_ParseTuple(args, "|b", &failover)) {
    return nullptr;
  }

  Status status = self->driver->stop(failover);
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_abort(MesosSchedulerDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status = self->driver->abort();
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_join(MesosSchedulerDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status;
  Py_BEGIN_ALLOW_THREADS
  status = self->driver->join();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_run(MesosSchedulerDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status;
  Py_BEGIN_ALLOW_THREADS
  status = self->driver->run();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_requestResources(
    MesosSchedulerDriverImpl* self,
    PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  PyObject* requestsObj = nullptr;
  vector<Request> requests;

  if (!PyArg_ParseTuple(args, "O", &requestsObj)) {
    return nullptr;
  }

  if (!PyList_Check(requestsObj)) {
    PyErr_Format(PyExc_Exception,
                 "Parameter 2 to requestsResources is not a list");
    return nullptr;
  }
  Py_ssize_t len = PyList_Size(requestsObj);
  for (int i = 0; i < len; i++) {
    PyObject* requestObj = PyList_GetItem(requestsObj, i);
    if (requestObj == nullptr) {
      return nullptr; // Exception will have been set by PyList_GetItem.
    }
    Request request;
    if (!readPythonProtobuf(requestObj, &request)) {
      PyErr_Format(PyExc_Exception, "Could not deserialize Python Request");
      return nullptr;
    }
    requests.push_back(request);
  }

  Status status = self->driver->requestResources(requests);
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_launchTasks(MesosSchedulerDriverImpl* self,
                                               PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  PyObject* offerIdsObj = nullptr;
  PyObject* tasksObj = nullptr;
  PyObject* filtersObj = nullptr;
  vector<OfferID> offerIds;
  vector<TaskInfo> tasks;
  Filters filters;

  if (!PyArg_ParseTuple(args, "OO|O", &offerIdsObj, &tasksObj, &filtersObj)) {
    return nullptr;
  }

  // Offer argument can be a list of offer ids or a single offer id (for
  // backward compatibility).
  if (!PyList_Check(offerIdsObj)) {
    OfferID offerId;
    if (!readPythonProtobuf(offerIdsObj, &offerId)) {
      PyErr_Format(PyExc_Exception, "Could not deserialize Python OfferID");
      return nullptr;
    }
    offerIds.push_back(offerId);
  } else {
    Py_ssize_t len = PyList_Size(offerIdsObj);
    for (int i = 0; i < len; i++) {
      PyObject* offerObj = PyList_GetItem(offerIdsObj, i);
      if (offerObj == nullptr) {
        return nullptr;
      }
      OfferID offerId;
      if (!readPythonProtobuf(offerObj, &offerId)) {
        PyErr_Format(PyExc_Exception,
                     "Could not deserialize Python OfferID");
        return nullptr;
      }
      offerIds.push_back(offerId);
    }
  }

  if (!PyList_Check(tasksObj)) {
    PyErr_Format(PyExc_Exception, "Parameter 2 to launchTasks is not a list");
    return nullptr;
  }
  Py_ssize_t len = PyList_Size(tasksObj);
  for (int i = 0; i < len; i++) {
    PyObject* taskObj = PyList_GetItem(tasksObj, i);
    if (taskObj == nullptr) {
      return nullptr; // Exception will have been set by PyList_GetItem.
    }
    TaskInfo task;
    if (!readPythonProtobuf(taskObj, &task)) {
      PyErr_Format(PyExc_Exception,
                   "Could not deserialize Python TaskInfo");
      return nullptr;
    }
    tasks.push_back(task);
  }

  if (filtersObj != nullptr) {
    if (!readPythonProtobuf(filtersObj, &filters)) {
      PyErr_Format(PyExc_Exception,
                   "Could not deserialize Python Filters");
      return nullptr;
    }
  }

  Status status = self->driver->launchTasks(offerIds, tasks, filters);
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_killTask(MesosSchedulerDriverImpl* self,
                                            PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  PyObject* tidObj = nullptr;
  TaskID tid;
  if (!PyArg_ParseTuple(args, "O", &tidObj)) {
    return nullptr;
  }
  if (!readPythonProtobuf(tidObj, &tid)) {
    PyErr_Format(PyExc_Exception, "Could not deserialize Python TaskID");
    return nullptr;
  }

  Status status = self->driver->killTask(tid);
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_acceptOffers(MesosSchedulerDriverImpl* self,
                                                PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  PyObject* offerIdsObj = nullptr;
  PyObject* operationsObj = nullptr;
  PyObject* filtersObj = nullptr;
  Py_ssize_t len = 0;
  vector<OfferID> offerIds;
  vector<Offer::Operation> operations;
  Filters filters;

  if (!PyArg_ParseTuple(args,
                        "OO|O",
                        &offerIdsObj,
                        &operationsObj,
                        &filtersObj)) {
    return nullptr;
  }

  if (!PyList_Check(offerIdsObj)) {
    PyErr_Format(PyExc_Exception, "Parameter 1 to acceptOffers is not a list");
    return nullptr;
  }

  len = PyList_Size(offerIdsObj);
  for (int i = 0; i < len; i++) {
    PyObject* offerObj = PyList_GetItem(offerIdsObj, i);
    if (offerObj == nullptr) {
      return nullptr;
    }

    OfferID offerId;
    if (!readPythonProtobuf(offerObj, &offerId)) {
      PyErr_Format(PyExc_Exception,
                   "Could not deserialize Python OfferID");
      return nullptr;
    }
    offerIds.push_back(offerId);
  }

  if (!PyList_Check(operationsObj)) {
    PyErr_Format(PyExc_Exception, "Parameter 2 to acceptOffers is not a list");
    return nullptr;
  }

  len = PyList_Size(operationsObj);
  for (int i = 0; i < len; i++) {
    PyObject* operationObj = PyList_GetItem(operationsObj, i);
    if (operationObj == nullptr) {
      return nullptr; // Exception will have been set by PyList_GetItem.
    }

    Offer::Operation operation;
    if (!readPythonProtobuf(operationObj, &operation)) {
      PyErr_Format(PyExc_Exception,
                   "Could not deserialize Python Offer.Operation");
      return nullptr;
    }
    operations.push_back(operation);
  }

  if (filtersObj != nullptr) {
    if (!readPythonProtobuf(filtersObj, &filters)) {
      PyErr_Format(PyExc_Exception,
                   "Could not deserialize Python Filters");
      return nullptr;
    }
  }

  Status status = self->driver->acceptOffers(offerIds, operations, filters);
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_declineOffer(MesosSchedulerDriverImpl* self,
                                                PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  PyObject* offerIdObj = nullptr;
  PyObject* filtersObj = nullptr;
  OfferID offerId;
  Filters filters;

  if (!PyArg_ParseTuple(args, "O|O", &offerIdObj, &filtersObj)) {
    return nullptr;
  }

  if (!readPythonProtobuf(offerIdObj, &offerId)) {
    PyErr_Format(PyExc_Exception, "Could not deserialize Python OfferID");
    return nullptr;
  }

  if (filtersObj != nullptr) {
    if (!readPythonProtobuf(filtersObj, &filters)) {
      PyErr_Format(PyExc_Exception,
                   "Could not deserialize Python Filters");
      return nullptr;
    }
  }

  Status status = self->driver->declineOffer(offerId, filters);
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_reviveOffers(MesosSchedulerDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status = self->driver->reviveOffers();
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_suppressOffers(
    MesosSchedulerDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status = self->driver->suppressOffers();
  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_acknowledgeStatusUpdate(
    MesosSchedulerDriverImpl* self,
    PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  PyObject* taskStatusObj = nullptr;
  TaskStatus taskStatus;

  if (!PyArg_ParseTuple(args, "O", &taskStatusObj)) {
    return nullptr;
  }

  if (!readPythonProtobuf(taskStatusObj, &taskStatus)) {
    PyErr_Format(PyExc_Exception, "Could not deserialize Python TaskStatus");
    return nullptr;
  }

  Status status = self->driver->acknowledgeStatusUpdate(taskStatus);

  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_sendFrameworkMessage(
    MesosSchedulerDriverImpl* self,
    PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  PyObject* slaveIdObj = nullptr;
  PyObject* executorIdObj = nullptr;
  SlaveID slaveId;
  ExecutorID executorId;
  const char* data;
  int length;

  if (!PyArg_ParseTuple(
      args, "OOs#", &executorIdObj, &slaveIdObj, &data, &length)) {
    return nullptr;
  }

  if (!readPythonProtobuf(executorIdObj, &executorId)) {
    PyErr_Format(PyExc_Exception, "Could not deserialize Python ExecutorID");
    return nullptr;
  }

  if (!readPythonProtobuf(slaveIdObj, &slaveId)) {
    PyErr_Format(PyExc_Exception, "Could not deserialize Python SlaveID");
    return nullptr;
  }

  Status status = self->driver->sendFrameworkMessage(
      executorId, slaveId, string(data, length));

  return PyInt_FromLong(status); // Sets exception if creating long fails.
}


PyObject* MesosSchedulerDriverImpl_reconcileTasks(
    MesosSchedulerDriverImpl* self,
    PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is nullptr");
    return nullptr;
  }

  PyObject* statusesObj = nullptr;
  vector<TaskStatus> statuses;

  if (!PyArg_ParseTuple(args, "O", &statusesObj)) {
    return nullptr;
  }

  if (!PyList_Check(statusesObj)) {
    PyErr_Format(PyExc_Exception,
                 "Parameter 1 to reconcileTasks is not a list");

    return nullptr;
  }

  Py_ssize_t len = PyList_Size(statusesObj);
  for (int i = 0; i < len; i++) {
    PyObject* statusObj = PyList_GetItem(statusesObj, i);
    if (statusObj == nullptr) {
      return nullptr;
    }

    TaskStatus status;
    if (!readPythonProtobuf(statusObj, &status)) {
      PyErr_Format(PyExc_Exception,
                   "Could not deserialize Python TaskStatus");
      return nullptr;
    }
    statuses.push_back(status);
  }

  Status status = self->driver->reconcileTasks(statuses);
  return PyInt_FromLong(status);
}

} // namespace python {
} // namespace mesos {
