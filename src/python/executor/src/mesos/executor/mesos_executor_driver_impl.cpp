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
#include "mesos_executor_driver_impl.hpp"
#include "proxy_executor.hpp"

using namespace mesos;
using namespace mesos::python;

using std::cerr;
using std::endl;
using std::string;
using std::vector;
using std::map;


namespace mesos { namespace python {

/**
 * Python type object for MesosExecutorDriverImpl.
 */
PyTypeObject MesosExecutorDriverImplType = {
  PyObject_HEAD_INIT(nullptr)
  0,                                               /* ob_size */
  "_mesos.MesosExecutorDriverImpl",                /* tp_name */
  sizeof(MesosExecutorDriverImpl),                 /* tp_basicsize */
  0,                                               /* tp_itemsize */
  (destructor) MesosExecutorDriverImpl_dealloc,    /* tp_dealloc */
  0,                                               /* tp_print */
  0,                                               /* tp_getattr */
  0,                                               /* tp_setattr */
  0,                                               /* tp_compare */
  0,                                               /* tp_repr */
  0,                                               /* tp_as_number */
  0,                                               /* tp_as_sequence */
  0,                                               /* tp_as_mapping */
  0,                                               /* tp_hash */
  0,                                               /* tp_call */
  0,                                               /* tp_str */
  0,                                               /* tp_getattro */
  0,                                               /* tp_setattro */
  0,                                               /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,         /* tp_flags */
  "Private MesosExecutorDriver implementation",    /* tp_doc */
  (traverseproc) MesosExecutorDriverImpl_traverse, /* tp_traverse */
  (inquiry) MesosExecutorDriverImpl_clear,         /* tp_clear */
  0,                                               /* tp_richcompare */
  0,                                               /* tp_weaklistoffset */
  0,                                               /* tp_iter */
  0,                                               /* tp_iternext */
  MesosExecutorDriverImpl_methods,                 /* tp_methods */
  0,                                               /* tp_members */
  0,                                               /* tp_getset */
  0,                                               /* tp_base */
  0,                                               /* tp_dict */
  0,                                               /* tp_descr_get */
  0,                                               /* tp_descr_set */
  0,                                               /* tp_dictoffset */
  (initproc) MesosExecutorDriverImpl_init,         /* tp_init */
  0,                                               /* tp_alloc */
  MesosExecutorDriverImpl_new,                     /* tp_new */
};


/**
 * List of Python methods in MesosExecutorDriverImpl.
 */
PyMethodDef MesosExecutorDriverImpl_methods[] = {
  { "start",
    (PyCFunction) MesosExecutorDriverImpl_start,
    METH_NOARGS,
    "Start the driver to connect to Mesos"
  },
  { "stop",
    (PyCFunction) MesosExecutorDriverImpl_stop,
    METH_NOARGS,
    "Stop the driver, disconnecting from Mesos"
  },
  { "abort",
    (PyCFunction) MesosExecutorDriverImpl_abort,
    METH_NOARGS,
    "Abort the driver, disallowing calls from and to the driver"
  },
  { "join",
    (PyCFunction) MesosExecutorDriverImpl_join,
    METH_NOARGS,
    "Wait for a running driver to disconnect from Mesos"
  },
  { "run",
    (PyCFunction) MesosExecutorDriverImpl_run,
    METH_NOARGS,
    "Start a driver and run it, returning when it disconnects from Mesos"
  },
  { "sendStatusUpdate",
    (PyCFunction) MesosExecutorDriverImpl_sendStatusUpdate,
    METH_VARARGS,
    "Send a status update for a task"
  },
  { "sendFrameworkMessage",
    (PyCFunction) MesosExecutorDriverImpl_sendFrameworkMessage,
    METH_VARARGS,
    "Send a FrameworkMessage to an agent"
  },
  { nullptr }  /* Sentinel */
};


/**
 * Create, but don't initialize, a new MesosExecutorDriverImpl
 * (called by Python before init method).
 */
PyObject* MesosExecutorDriverImpl_new(PyTypeObject *type,
                                      PyObject *args,
                                      PyObject *kwds)
{
  MesosExecutorDriverImpl *self;
  self = (MesosExecutorDriverImpl *) type->tp_alloc(type, 0);
  if (self != nullptr) {
    self->driver = nullptr;
    self->proxyExecutor = nullptr;
    self->pythonExecutor = nullptr;
  }
  return (PyObject*) self;
}


/**
 * Initialize a MesosExecutorDriverImpl with constructor arguments.
 */
int MesosExecutorDriverImpl_init(MesosExecutorDriverImpl *self,
                                 PyObject *args,
                                 PyObject *kwds)
{
  PyObject *pythonExecutor = nullptr;

  if (!PyArg_ParseTuple(args, "O", &pythonExecutor)) {
    return -1;
  }

  if (pythonExecutor != nullptr) {
    PyObject* tmp = self->pythonExecutor;
    Py_INCREF(pythonExecutor);
    self->pythonExecutor = pythonExecutor;
    Py_XDECREF(tmp);
  }

  if (self->driver != nullptr) {
    delete self->driver;
    self->driver = nullptr;
  }

  if (self->proxyExecutor != nullptr) {
    delete self->proxyExecutor;
    self->proxyExecutor = nullptr;
  }

  self->proxyExecutor = new ProxyExecutor(self);
  self->driver = new MesosExecutorDriver(self->proxyExecutor);

  return 0;
}


/**
 * Free a MesosExecutorDriverImpl.
 */
void MesosExecutorDriverImpl_dealloc(MesosExecutorDriverImpl* self)
{
  if (self->driver != nullptr) {
    // We need to wrap the driver destructor in an "allow threads"
    // macro since the MesosExecutorDriver destructor waits for the
    // ExecutorProcess to terminate and there might be a thread that
    // is trying to acquire the GIL to call through the
    // ProxyExecutor. It will only be after this thread executes that
    // the ExecutorProcess might actually get a terminate.
    Py_BEGIN_ALLOW_THREADS
    delete self->driver;
    Py_END_ALLOW_THREADS
    self->driver = nullptr;
  }

  if (self->proxyExecutor != nullptr) {
    delete self->proxyExecutor;
    self->proxyExecutor = nullptr;
  }

  MesosExecutorDriverImpl_clear(self);
  self->ob_type->tp_free((PyObject*) self);
}


/**
 * Traverse fields of a MesosExecutorDriverImpl on a cyclic GC search.
 * See http://docs.python.org/extending/newtypes.html.
 */
int MesosExecutorDriverImpl_traverse(MesosExecutorDriverImpl* self,
                                     visitproc visit,
                                     void* arg)
{
  Py_VISIT(self->pythonExecutor);
  return 0;
}


/**
 * Clear fields of a MesosExecutorDriverImpl that can participate in
 * GC cycles. See http://docs.python.org/extending/newtypes.html.
 */
int MesosExecutorDriverImpl_clear(MesosExecutorDriverImpl* self)
{
  Py_CLEAR(self->pythonExecutor);
  return 0;
}


PyObject* MesosExecutorDriverImpl_start(MesosExecutorDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status = self->driver->start();
  return PyInt_FromLong(status); // Sets an exception if creating the int fails.
}


PyObject* MesosExecutorDriverImpl_stop(MesosExecutorDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status = self->driver->stop();
  return PyInt_FromLong(status); // Sets an exception if creating the int fails.
}


PyObject* MesosExecutorDriverImpl_abort(MesosExecutorDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status = self->driver->abort();
  return PyInt_FromLong(status); // Sets an exception if creating the int fails.
}


PyObject* MesosExecutorDriverImpl_join(MesosExecutorDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status;
  Py_BEGIN_ALLOW_THREADS
  status = self->driver->join();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(status); // Sets an exception if creating the int fails.
}


PyObject* MesosExecutorDriverImpl_run(MesosExecutorDriverImpl* self)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is nullptr");
    return nullptr;
  }

  Status status;
  Py_BEGIN_ALLOW_THREADS
  status = self->driver->run();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(status); // Sets an exception if creating the int fails.
}


PyObject* MesosExecutorDriverImpl_sendStatusUpdate(
    MesosExecutorDriverImpl* self,
    PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is nullptr");
    return nullptr;
  }

  PyObject* statusObj = nullptr;
  TaskStatus taskStatus;
  if (!PyArg_ParseTuple(args, "O", &statusObj)) {
    return nullptr;
  }
  if (!readPythonProtobuf(statusObj, &taskStatus)) {
    PyErr_Format(PyExc_Exception,
                 "Could not deserialize Python TaskStatus");
    return nullptr;
  }

  Status status = self->driver->sendStatusUpdate(taskStatus);
  return PyInt_FromLong(status); // Sets an exception if creating the int fails.
}


PyObject* MesosExecutorDriverImpl_sendFrameworkMessage(
    MesosExecutorDriverImpl* self,
    PyObject* args)
{
  if (self->driver == nullptr) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is nullptr");
    return nullptr;
  }

  const char* data;
  int length;
  if (!PyArg_ParseTuple(args, "s#", &data, &length)) {
    return nullptr;
  }

  Status status = self->driver->sendFrameworkMessage(string(data, length));
  return PyInt_FromLong(status); // Sets an exception if creating the int fails.
}

} // namespace python {
} // namespace mesos {
