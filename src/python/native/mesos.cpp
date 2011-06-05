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

#include <Python.h>

#include <iostream>

#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "mesos_sched.hpp"
#include "mesos_exec.hpp"

using std::cout;
using std::cerr;
using std::endl;
using namespace mesos;


namespace {

/**
 * Method list for our Python module.
 */
PyMethodDef MODULE_METHODS[] = {
  {NULL, NULL, 0, NULL}        /* Sentinel */
};


/**
 * RAAI utility class for acquiring the Python global interpreter lock.
 */
class InterpreterLock {
  PyGILState_STATE state;

public:
  InterpreterLock() {
    state = PyGILState_Ensure();
  }

  ~InterpreterLock() {
    PyGILState_Release(state);
  }
};


/**
 * Convert a Python protocol buffer object into a C++ one by serializing
 * it to a string and deserializing the result back in C++. Returns true
 * on success, or prints an error and returns false on failure.
 */
template <typename T>
bool readProtobufObject(PyObject* obj, T* t)
{
  if (obj == Py_None) {
    cerr << "None object given where protobuf expected" << endl;
    return false;
  }
  PyObject* res = PyObject_CallMethod(obj,
                                      (char*) "SerializeToString",
                                      (char*) NULL);
  if (res == NULL) {
    cerr << "Failed to call Python object's SerializeToString "
         << "(perhaps it is not a protobuf?)" << endl;
    PyErr_Print();
    return false;
  }
  char* chars;
  Py_ssize_t len;
  if (PyString_AsStringAndSize(res, &chars, &len) < 0) {
    cerr << "SerializeToString did not return a string" << endl;
    PyErr_Print();
    Py_DECREF(res);
    return false;
  }
  google::protobuf::io::ArrayInputStream stream(chars, len);
  bool success = t->ParseFromZeroCopyStream(&stream);
  if (!success) {
    cerr << "Could not deserialize protobuf as expected type" << endl;
  }
  Py_DECREF(res);
  return success;
}


class ProxyScheduler;


/**
 * Python object structure for MesosSchedulerDriverImpl objects.
 */
struct MesosSchedulerDriverImpl {
    PyObject_HEAD
    /* Type-specific fields go here. */
    MesosSchedulerDriver* driver;
    ProxyScheduler* proxyScheduler;
    PyObject* pythonScheduler;
};


/**
 * Proxy Scheduler implementation that will call into Python
 */
class ProxyScheduler : public Scheduler
{
  MesosSchedulerDriverImpl *impl;

public:
  ProxyScheduler(MesosSchedulerDriverImpl *_impl) : impl(_impl) {}

  virtual ~ProxyScheduler() {}

  // Callbacks for getting framework properties.
  virtual std::string getFrameworkName(SchedulerDriver* driver);
  virtual ExecutorInfo getExecutorInfo(SchedulerDriver* driver);

  // Callbacks for various Mesos events.
  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId) {};

  virtual void resourceOffer(SchedulerDriver* driver,
                             const OfferID& offerId,
                             const std::vector<SlaveOffer>& offers) {};

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) {};

  virtual void statusUpdate(SchedulerDriver* driver,
                            const TaskStatus& status) {};

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const FrameworkMessage& message) {};

  virtual void slaveLost(SchedulerDriver* driver,
                         const SlaveID& sid) {};

  virtual void error(SchedulerDriver* driver,
                     int code,
                     const std::string& message) {};

};


/**
 * Create, but don't initialize, a new MesosSchedulerDriverImpl
 * (called by Python before init method).
 */
PyObject* MesosSchedulerDriverImpl_new(PyTypeObject *type,
                                       PyObject *args,
                                       PyObject *kwds)
{
  cout << "In MesosSchedulerDriverImpl_new" << endl;
  MesosSchedulerDriverImpl *self;
  self = (MesosSchedulerDriverImpl *) type->tp_alloc(type, 0);
  if (self != NULL) {
    self->driver = NULL;
    self->proxyScheduler = NULL;
    self->pythonScheduler = NULL;
  }
  return (PyObject*) self;
}


void MesosSchedulerDriverImpl_dealloc(MesosSchedulerDriverImpl* self)
{
  cout << "In MesosSchedulerDriverImpl_dealloc" << endl;
  if (self->driver != NULL) {
    self->driver->stop();
    delete self->driver;
  }
  if (self->proxyScheduler != NULL) {
    delete self->proxyScheduler;
  }
  Py_XDECREF(self->pythonScheduler);
  self->ob_type->tp_free((PyObject*) self);
}


int MesosSchedulerDriverImpl_init(MesosSchedulerDriverImpl *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
  cout << "In MesosSchedulerDriverImpl_init" << endl;
  PyObject *pythonScheduler = NULL;
  const char* url;
  PyObject *frameworkId = NULL;

  if (!PyArg_ParseTuple(args, "Os|O", &pythonScheduler, &url, &frameworkId)) {
    return -1;
  }

  if (pythonScheduler != NULL) {
    PyObject* tmp = self->pythonScheduler;
    Py_INCREF(pythonScheduler);
    self->pythonScheduler = pythonScheduler;
    Py_XDECREF(tmp);
  }

  if (self->driver != NULL) {
    delete self->driver;
    self->driver = NULL;
  }

  if (self->proxyScheduler != NULL) {
    delete self->proxyScheduler;
    self->proxyScheduler = NULL;
  }

  if (frameworkId != NULL) {
  } else {
  }

  self->proxyScheduler = new ProxyScheduler(self);
  self->driver = new MesosSchedulerDriver(self->proxyScheduler, url);
  cout << "Created self->driver: " << self->driver << endl;

  return 0;
}


PyObject* MesosSchedulerDriverImpl_start(MesosSchedulerDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_SetString(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  int res = self->driver->start();
  return PyInt_FromLong(res);
}


PyObject* MesosSchedulerDriverImpl_stop(MesosSchedulerDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_SetString(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  int res = self->driver->stop();
  return PyInt_FromLong(res);
}


PyObject* MesosSchedulerDriverImpl_join(MesosSchedulerDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_SetString(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  int res;
  Py_BEGIN_ALLOW_THREADS
  res = self->driver->join();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(res);
}


PyObject* MesosSchedulerDriverImpl_run(MesosSchedulerDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_SetString(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  int res;
  Py_BEGIN_ALLOW_THREADS
  res = self->driver->run();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(res);
}


std::string ProxyScheduler::getFrameworkName(SchedulerDriver* driver) {
  cout << "ProxyScheduler::getFrameworkName being called" << endl;
  InterpreterLock lock;
  PyObject* res = PyObject_CallMethod(impl->pythonScheduler,
                                      (char*) "getFrameworkName",
                                      (char*) "O",
                                      impl);
  if (res == NULL) {
    cerr << "Failed to call scheduler's getFrameworkName" << endl;
    PyErr_Print();
    driver->stop();
    return "";
  }
  if (res == Py_None) {
    cerr << "Scheduler's getFrameworkName returned None" << endl;
    driver->stop();
    return "";
  }
  char* chars = PyString_AsString(res);
  if (chars == NULL) {
    cerr << "Scheduler's getFrameworkName did not return a string" << endl;
    PyErr_Print();
    driver->stop();
    Py_DECREF(res);
    return "";
  }
  std::string str(chars);
  Py_DECREF(res);
  cout << "Returning " << str << endl;
  return str;
};


ExecutorInfo ProxyScheduler::getExecutorInfo(SchedulerDriver* driver) {
  cout << "ProxyScheduler::getExecutorInfo being called" << endl;
  InterpreterLock lock;
  ExecutorInfo info;
  PyObject* res = PyObject_CallMethod(impl->pythonScheduler,
                                      (char*) "getExecutorInfo",
                                      (char*) "O",
                                      impl);
  if (res == NULL) {
    cerr << "Failed to call scheduler's getExecutorInfo" << endl;
    PyErr_Print();
    driver->stop();
    return info;
  }
  if (res == Py_None) {
    cerr << "Scheduler's getExecutorInfo returned None" << endl;
    driver->stop();
    Py_DECREF(res);
    return info;
  }
  if (!readProtobufObject<ExecutorInfo>(res, &info)) {
    cerr << "Could not deserialize Python ExecutorInfo" << endl;
    driver->stop();
  }
  Py_DECREF(res);
  return info;
};


PyMethodDef MesosSchedulerDriverImpl_methods[] = {
  {"start", (PyCFunction) MesosSchedulerDriverImpl_start, METH_NOARGS,
   "Start the driver to connect to Mesos"},
  {"stop", (PyCFunction) MesosSchedulerDriverImpl_stop, METH_NOARGS,
   "Stop the driver, disconnecting from Mesos"},
  {"join", (PyCFunction) MesosSchedulerDriverImpl_join, METH_NOARGS,
   "Wait for a running driver to disconnect from Mesos"},
  {"run", (PyCFunction) MesosSchedulerDriverImpl_run, METH_NOARGS,
   "Start a driver and run it, returning when it disconnects from Mesos"},
  {NULL}  /* Sentinel */
};


/**
 * Python type object for MesosSchedulerDriverImpl.
 */
PyTypeObject MesosSchedulerDriverImplType = {
    PyObject_HEAD_INIT(NULL)
    0,                                             /* ob_size */
    "_mesos.MesosSchedulerDriverImpl",             /* tp_name */
    sizeof(MesosSchedulerDriverImpl),              /* tp_basicsize */
    0,                                             /* tp_itemsize */
    (destructor) MesosSchedulerDriverImpl_dealloc, /* tp_dealloc */
    0,                                             /* tp_print */
    0,                                             /* tp_getattr */
    0,                                             /* tp_setattr */
    0,                                             /* tp_compare */
    0,                                             /* tp_repr */
    0,                                             /* tp_as_number */
    0,                                             /* tp_as_sequence */
    0,                                             /* tp_as_mapping */
    0,                                             /* tp_hash */
    0,                                             /* tp_call */
    0,                                             /* tp_str */
    0,                                             /* tp_getattro */
    0,                                             /* tp_setattro */
    0,                                             /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                            /* tp_flags */
    "Private MesosSchedulerDriver implementation", /* tp_doc */
    0,		                                   /* tp_traverse */
    0,		                                   /* tp_clear */
    0,		                                   /* tp_richcompare */
    0,		                                   /* tp_weaklistoffset */
    0,		                                   /* tp_iter */
    0,		                                   /* tp_iternext */
    MesosSchedulerDriverImpl_methods,              /* tp_methods */
    0,                                             /* tp_members */
    0,                                             /* tp_getset */
    0,                                             /* tp_base */
    0,                                             /* tp_dict */
    0,                                             /* tp_descr_get */
    0,                                             /* tp_descr_set */
    0,                                             /* tp_dictoffset */
    (initproc) MesosSchedulerDriverImpl_init,      /* tp_init */
    0,                                             /* tp_alloc */
    MesosSchedulerDriverImpl_new,                  /* tp_new */
};

} /* end namespace */


/**
 * Called by Python to initialize our module.
 */
PyMODINIT_FUNC init_mesos(void)
{
  if (PyType_Ready(&MesosSchedulerDriverImplType) < 0)
    return;

  PyObject* module = Py_InitModule("_mesos", MODULE_METHODS);

  Py_INCREF(&MesosSchedulerDriverImplType);
  PyModule_AddObject(module,
                     "MesosSchedulerDriverImpl",
                     (PyObject*) &MesosSchedulerDriverImplType);
}
