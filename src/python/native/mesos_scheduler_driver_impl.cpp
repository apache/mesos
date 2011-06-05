#include <Python.h>

#include "mesos_scheduler_driver_impl.hpp"
#include "module.hpp"
#include "proxy_scheduler.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::vector;
using std::map;
using namespace mesos;
using namespace mesos::python;

namespace mesos { namespace python {

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
 * Initialize a MesosSchedulerDriverImpl with constructor arguments.
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


/**
 * Initialize a MesosSchedulerDriverImpl (this is its constructor).
 */
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


/**
 * Free a MesosSchedulerDriverImpl.
 */
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

}} /* namespace mesos { namespace python { */
