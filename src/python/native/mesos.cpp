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
using std::string;
using std::vector;
using std::map;
using namespace mesos;


namespace {

/**
 * Method list for our Python module.
 */
PyMethodDef MODULE_METHODS[] = {
  {NULL, NULL, 0, NULL}        /* Sentinel */
};


/**
 * The Python module object for mesos_pb2 (which contains the protobuf
 * classes generated for Python).
 */
PyObject* mesos_pb2 = NULL;


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
bool readPythonProtobuf(PyObject* obj, T* t)
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


/**
 * Convert a C++ protocol buffer object into a Python one by serializing
 * it to a string and deserializing the result back in Python. Returns the
 * resulting PyObject* on success or raises a Python exception and returns
 * NULL on failure.
 */
template <typename T>
PyObject* createPythonProtobuf(const T& t, const char* typeName)
{
  PyObject* dict = PyModule_GetDict(mesos_pb2);
  if (dict == NULL) {
    PyErr_Format(PyExc_Exception, "PyModule_GetDict failed");
    return NULL;
  }

  PyObject* type = PyDict_GetItemString(dict, typeName);
  if (type == NULL) {
    PyErr_Format(PyExc_Exception, "Could not resolve mesos_pb2.%s", typeName);
    return NULL;
  }
  if (!PyType_Check(type)) {
    PyErr_Format(PyExc_Exception, "mesos_pb2.%s is not a type", typeName);
    return NULL;
  }

  string str;
  if (!t.SerializeToString(&str)) {
    PyErr_Format(PyExc_Exception, "C++ %s SerializeToString failed", typeName);
    return NULL;
  }

  // Propagates any exception that might happen in FromString
  return PyObject_CallMethod(type,
                             (char*) "FromString",
                             (char*) "s#",
                             str.data(),
                             str.size());
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
  virtual string getFrameworkName(SchedulerDriver* driver);
  virtual ExecutorInfo getExecutorInfo(SchedulerDriver* driver);

  // Callbacks for various Mesos events.
  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId);

  virtual void resourceOffer(SchedulerDriver* driver,
                             const OfferID& offerId,
                             const vector<SlaveOffer>& offers);

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId);

  virtual void statusUpdate(SchedulerDriver* driver,
                            const TaskStatus& status);

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const FrameworkMessage& message);

  virtual void slaveLost(SchedulerDriver* driver,
                         const SlaveID& slaveId);

  virtual void error(SchedulerDriver* driver,
                     int code,
                     const string& message);

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
    self->driver->stop();
    delete self->driver;
    self->driver = NULL;
  }

  if (self->proxyScheduler != NULL) {
    delete self->proxyScheduler;
    self->proxyScheduler = NULL;
  }

  FrameworkID fid;
  if (frameworkId != NULL) {
    if (!readPythonProtobuf(frameworkId, &fid)) {
      PyErr_Format(PyExc_Exception, "Could not deserialize Python FrameworkId");
      return -1;
    }
  }

  self->proxyScheduler = new ProxyScheduler(self);

  if (frameworkId != NULL) {
    self->driver = new MesosSchedulerDriver(self->proxyScheduler, url, fid);
  } else {
    self->driver = new MesosSchedulerDriver(self->proxyScheduler, url);
  }

  return 0;
}


PyObject* MesosSchedulerDriverImpl_start(MesosSchedulerDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  int res = self->driver->start();
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosSchedulerDriverImpl_stop(MesosSchedulerDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  int res = self->driver->stop();
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosSchedulerDriverImpl_join(MesosSchedulerDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  int res;
  Py_BEGIN_ALLOW_THREADS
  res = self->driver->join();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosSchedulerDriverImpl_run(MesosSchedulerDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  int res;
  Py_BEGIN_ALLOW_THREADS
  res = self->driver->run();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosSchedulerDriverImpl_reviveOffers(MesosSchedulerDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  int res = self->driver->reviveOffers();
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosSchedulerDriverImpl_replyToOffer(MesosSchedulerDriverImpl* self,
                                                PyObject* args)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  PyObject* oidObj = NULL;
  PyObject* tasksObj = NULL;
  PyObject* paramsObj = NULL;
  OfferID oid;
  map<string, string> params;
  vector<TaskDescription> tasks;

  if (!PyArg_ParseTuple(args, "OO|O", &oidObj, &tasksObj, &paramsObj)) {
    return NULL;
  }

  if (!readPythonProtobuf(oidObj, &oid)) {
    PyErr_Format(PyExc_Exception, "Could not deserialize Python OfferID");
    return NULL;
  }

  if (!PyList_Check(tasksObj)) {
    PyErr_Format(PyExc_Exception, "Parameter 2 to replyToOffer is not a list");
    return NULL;
  }
  Py_ssize_t len = PyList_Size(tasksObj);
  for (int i = 0; i < len; i++) {
    PyObject* taskObj = PyList_GetItem(tasksObj, i);
    if (tasksObj == NULL) {
      return NULL; // Exception will have been set by PyList_GetItem
    }
    TaskDescription task;
    if (!readPythonProtobuf(taskObj, &task)) {
      PyErr_Format(PyExc_Exception,
                   "Could not deserialize Python TaskDescription");
      return NULL;
    }
    tasks.push_back(task);
  }

  if (paramsObj != NULL) {
    if (!PyDict_Check(paramsObj)) {
      PyErr_Format(PyExc_Exception,
                   "Parameter 3 to replyToOffer is not a dictionary");
      return NULL;
    }

    Py_ssize_t pos = 0;
    PyObject* key;
    PyObject* value;
    while (PyDict_Next(paramsObj, &pos, &key, &value)) {
      // Convert both key and value to strings. Note that this returns
      // new references, which must be cleaned up.
      PyObject* keyStr = PyObject_Str(key);
      if (keyStr == NULL) {
        return NULL;
      }
      PyObject* valueStr = PyObject_Str(value);
      if (valueStr == NULL) {
        Py_DECREF(keyStr);
        return NULL;
      }
      char* keyChars = PyString_AsString(keyStr);
      if (keyChars == NULL) {
        Py_DECREF(keyStr);
        Py_DECREF(valueStr);
        return NULL;
      }
      char* valueChars = PyString_AsString(valueStr);
      if (valueChars == NULL) {
        Py_DECREF(keyStr);
        Py_DECREF(valueStr);
        return NULL;
      }
      params[keyChars] = valueChars;
      Py_DECREF(keyStr);
      Py_DECREF(valueStr);
    }
  }

  int res;
  if (paramsObj != NULL) {
    res = self->driver->replyToOffer(oid, tasks, params);
  } else {
    res = self->driver->replyToOffer(oid, tasks);
  }
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosSchedulerDriverImpl_killTask(MesosSchedulerDriverImpl* self,
                                            PyObject* args)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  PyObject* tidObj = NULL;
  TaskID tid;
  if (!PyArg_ParseTuple(args, "O", &tidObj)) {
    return NULL;
  }
  if (!readPythonProtobuf(tidObj, &tid)) {
    PyErr_Format(PyExc_Exception, "Could not deserialize Python TaskID");
    return NULL;
  }

  int res = self->driver->killTask(tid);
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosSchedulerDriverImpl_sendFrameworkMessage(
    MesosSchedulerDriverImpl* self,
    PyObject* args)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosSchedulerDriverImpl.driver is NULL");
    return NULL;
  }

  PyObject* msgObj = NULL;
  FrameworkMessage msg;
  if (!PyArg_ParseTuple(args, "O", &msgObj)) {
    return NULL;
  }
  if (!readPythonProtobuf(msgObj, &msg)) {
    PyErr_Format(PyExc_Exception,
                 "Could not deserialize Python FrameworkMessage");
    return NULL;
  }

  int res = self->driver->sendFrameworkMessage(msg);
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


string ProxyScheduler::getFrameworkName(SchedulerDriver* driver) {
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
  string str(chars);
  Py_DECREF(res);
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
    goto cleanup;
  }
  if (res == Py_None) {
    PyErr_Format(PyExc_Exception, "Scheduler's getExecutorInfo returned None");
    goto cleanup;
  }
  if (!readPythonProtobuf(res, &info)) {
    PyErr_Format(PyExc_Exception, "Could not deserialize Python ExecutorInfo");
    goto cleanup;
  }
cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->stop();
  }
  Py_XDECREF(res);
  return info;
};


// Callbacks for various Mesos events.
void ProxyScheduler::registered(SchedulerDriver* driver,
                                const FrameworkID& frameworkId)
{
  cout << "ProxyScheduler::registered being called" << endl;
  InterpreterLock lock;
  
  PyObject* fid = NULL;
  PyObject* res = NULL;
  
  fid = createPythonProtobuf(frameworkId, "FrameworkID");
  if (fid == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "registered",
                            (char*) "OO",
                            impl,
                            fid);
  if (res == NULL) {
    cerr << "Failed to call scheduler's registered" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->stop();
  }
  Py_XDECREF(fid);
  Py_XDECREF(res);
}


void ProxyScheduler::resourceOffer(SchedulerDriver* driver,
                                   const OfferID& offerId,
                                   const vector<SlaveOffer>& offers)
{
  cout << "ProxyScheduler::resourceOffer being called" << endl;
  InterpreterLock lock;

  PyObject* oid = NULL;
  PyObject* list = NULL;
  PyObject* res = NULL;

  oid = createPythonProtobuf(offerId, "OfferID");
  if (oid == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception
  }

  list = PyList_New(offers.size());
  if (list == NULL) {
    goto cleanup;
  }
  for (int i = 0; i < offers.size(); i++) {
    PyObject* offer = createPythonProtobuf(offers[i], "SlaveOffer");
    if (offer == NULL) {
      goto cleanup;
    }
    PyList_SetItem(list, i, offer); // Steals the reference to offer
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "resourceOffer",
                            (char*) "OOO",
                            impl,
                            oid,
                            list);
  if (res == NULL) {
    cerr << "Failed to call scheduler's resourceOffer" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->stop();
  }
  Py_XDECREF(oid);
  Py_XDECREF(list);
  Py_XDECREF(res);
}


void ProxyScheduler::offerRescinded(SchedulerDriver* driver,
                                    const OfferID& offerId)
{
  cout << "ProxyScheduler::offerRescinded being called" << endl;
  InterpreterLock lock;
  
  PyObject* oid = NULL;
  PyObject* res = NULL;
  
  oid = createPythonProtobuf(offerId, "OfferID");
  if (oid == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception
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
    driver->stop();
  }
  Py_XDECREF(oid);
  Py_XDECREF(res);
}


void ProxyScheduler::statusUpdate(SchedulerDriver* driver,
                                  const TaskStatus& status)
{
  cout << "ProxyScheduler::statusUpdate being called" << endl;
  InterpreterLock lock;
  
  PyObject* stat = NULL;
  PyObject* res = NULL;
  
  stat = createPythonProtobuf(status, "TaskStatus");
  if (stat == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception
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
    driver->stop();
  }
  Py_XDECREF(stat);
  Py_XDECREF(res);
}


void ProxyScheduler::frameworkMessage(SchedulerDriver* driver,
                                      const FrameworkMessage& message)
{
  cout << "ProxyScheduler::frameworkMessage being called" << endl;
  InterpreterLock lock;
  
  PyObject* msg = NULL;
  PyObject* res = NULL;
  
  msg = createPythonProtobuf(message, "FrameworkMessage");
  if (msg == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception
  }

  res = PyObject_CallMethod(impl->pythonScheduler,
                            (char*) "frameworkMessage",
                            (char*) "OO",
                            impl,
                            msg);
  if (res == NULL) {
    cerr << "Failed to call scheduler's frameworkMessage" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->stop();
  }
  Py_XDECREF(msg);
  Py_XDECREF(res);
}


void ProxyScheduler::slaveLost(SchedulerDriver* driver,
                               const SlaveID& slaveId)
{
  cout << "ProxyScheduler::slaveLost being called" << endl;
  InterpreterLock lock;
  
  PyObject* sid = NULL;
  PyObject* res = NULL;
  
  sid = createPythonProtobuf(slaveId, "SlaveID");
  if (sid == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception
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
    driver->stop();
  }
  Py_XDECREF(sid);
  Py_XDECREF(res);
}


void ProxyScheduler::error(SchedulerDriver* driver,
                           int code,
                           const string& message)
{
  cout << "ProxyScheduler::error being called" << endl;
  InterpreterLock lock;
  PyObject* res = PyObject_CallMethod(impl->pythonScheduler,
                                      (char*) "error",
                                      (char*) "Ois",
                                      impl,
                                      code,
                                      message.c_str());
  if (res == NULL) {
    cerr << "Failed to call scheduler's error" << endl;
    goto cleanup;
  }
cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    // No need for driver.stop(); it should stop itself
  }
  Py_XDECREF(res);
}


/**
 * List of Python methods in MesosSchedulerDriverImpl.
 */
PyMethodDef MesosSchedulerDriverImpl_methods[] = {
  {"start", (PyCFunction) MesosSchedulerDriverImpl_start, METH_NOARGS,
   "Start the driver to connect to Mesos"},
  {"stop", (PyCFunction) MesosSchedulerDriverImpl_stop, METH_NOARGS,
   "Stop the driver, disconnecting from Mesos"},
  {"join", (PyCFunction) MesosSchedulerDriverImpl_join, METH_NOARGS,
   "Wait for a running driver to disconnect from Mesos"},
  {"run", (PyCFunction) MesosSchedulerDriverImpl_run, METH_NOARGS,
   "Start a driver and run it, returning when it disconnects from Mesos"},
  {"replyToOffer",
   (PyCFunction) MesosSchedulerDriverImpl_replyToOffer,
   METH_VARARGS,
   "Reply to a Mesos offer with a list of tasks"},
  {"killTask",
   (PyCFunction) MesosSchedulerDriverImpl_killTask,
   METH_VARARGS,
   "Kill the task with the given ID"},
  {"sendFrameworkMessage",
   (PyCFunction) MesosSchedulerDriverImpl_sendFrameworkMessage,
   METH_VARARGS,
   "Send a FrameworkMessage to a slave"},
  {"reviveOffers",
   (PyCFunction) MesosSchedulerDriverImpl_reviveOffers,
   METH_NOARGS,
   "Remove all filters and ask Mesos for new offers"},
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
  0,                                             /* tp_traverse */
  0,                                             /* tp_clear */
  0,                                             /* tp_richcompare */
  0,                                             /* tp_weaklistoffset */
  0,                                             /* tp_iter */
  0,                                             /* tp_iternext */
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
  // Ensure that the interpreter's threading support is enabled
  PyEval_InitThreads();

  // Import the mesos_pb2 module (on which we depend for protobuf classes)
  mesos_pb2 = PyImport_ImportModule("mesos_pb2");
  if (mesos_pb2 == NULL)
    return;

  // Initialize the MesosSchedulerDriverImpl type
  if (PyType_Ready(&MesosSchedulerDriverImplType) < 0)
    return;

  // Create the _mesos module and add our types to it
  PyObject* module = Py_InitModule("_mesos", MODULE_METHODS);

  Py_INCREF(&MesosSchedulerDriverImplType);
  PyModule_AddObject(module,
                     "MesosSchedulerDriverImpl",
                     (PyObject*) &MesosSchedulerDriverImplType);
}
