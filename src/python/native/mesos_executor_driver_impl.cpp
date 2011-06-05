#include <Python.h>

#include "mesos_executor_driver_impl.hpp"
#include "module.hpp"
#include "proxy_executor.hpp"

using std::cerr;
using std::endl;
using std::string;
using std::vector;
using std::map;
using namespace mesos;
using namespace mesos::python;

namespace mesos { namespace python {

/**
 * Python type object for MesosExecutorDriverImpl.
 */
PyTypeObject MesosExecutorDriverImplType = {
  PyObject_HEAD_INIT(NULL)
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
  {"start", (PyCFunction) MesosExecutorDriverImpl_start, METH_NOARGS,
   "Start the driver to connect to Mesos"},
  {"stop", (PyCFunction) MesosExecutorDriverImpl_stop, METH_NOARGS,
   "Stop the driver, disconnecting from Mesos"},
  {"join", (PyCFunction) MesosExecutorDriverImpl_join, METH_NOARGS,
   "Wait for a running driver to disconnect from Mesos"},
  {"run", (PyCFunction) MesosExecutorDriverImpl_run, METH_NOARGS,
   "Start a driver and run it, returning when it disconnects from Mesos"},
  {"sendStatusUpdate",
   (PyCFunction) MesosExecutorDriverImpl_sendStatusUpdate,
   METH_VARARGS,
   "Send a status update for a task"},
  {"sendFrameworkMessage",
   (PyCFunction) MesosExecutorDriverImpl_sendFrameworkMessage,
   METH_VARARGS,
   "Send a FrameworkMessage to a slave"},
  {NULL}  /* Sentinel */
};


/**
 * Initialize a MesosExecutorDriverImpl with constructor arguments.
 */
PyObject* MesosExecutorDriverImpl_new(PyTypeObject *type,
                                       PyObject *args,
                                       PyObject *kwds)
{
  MesosExecutorDriverImpl *self;
  self = (MesosExecutorDriverImpl *) type->tp_alloc(type, 0);
  if (self != NULL) {
    self->driver = NULL;
    self->proxyExecutor = NULL;
    self->pythonExecutor = NULL;
  }
  return (PyObject*) self;
}


/**
 * Initialize a MesosExecutorDriverImpl (this is its constructor).
 */
int MesosExecutorDriverImpl_init(MesosExecutorDriverImpl *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
  PyObject *pythonExecutor = NULL;

  if (!PyArg_ParseTuple(args, "O", &pythonExecutor)) {
    return -1;
  }

  if (pythonExecutor != NULL) {
    PyObject* tmp = self->pythonExecutor;
    Py_INCREF(pythonExecutor);
    self->pythonExecutor = pythonExecutor;
    Py_XDECREF(tmp);
  }

  if (self->driver != NULL) {
    self->driver->stop();
    delete self->driver;
    self->driver = NULL;
  }

  if (self->proxyExecutor != NULL) {
    delete self->proxyExecutor;
    self->proxyExecutor = NULL;
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
  if (self->driver != NULL) {
    self->driver->stop();
    delete self->driver;
    self->driver = NULL;
  }
  if (self->proxyExecutor != NULL) {
    delete self->proxyExecutor;
    self->proxyExecutor = NULL;
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
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is NULL");
    return NULL;
  }

  int res = self->driver->start();
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosExecutorDriverImpl_stop(MesosExecutorDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is NULL");
    return NULL;
  }

  int res = self->driver->stop();
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosExecutorDriverImpl_join(MesosExecutorDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is NULL");
    return NULL;
  }

  int res;
  Py_BEGIN_ALLOW_THREADS
  res = self->driver->join();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosExecutorDriverImpl_run(MesosExecutorDriverImpl* self)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is NULL");
    return NULL;
  }

  int res;
  Py_BEGIN_ALLOW_THREADS
  res = self->driver->run();
  Py_END_ALLOW_THREADS
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosExecutorDriverImpl_sendStatusUpdate(
    MesosExecutorDriverImpl* self,
    PyObject* args)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is NULL");
    return NULL;
  }

  PyObject* statusObj = NULL;
  TaskStatus status;
  if (!PyArg_ParseTuple(args, "O", &statusObj)) {
    return NULL;
  }
  if (!readPythonProtobuf(statusObj, &status)) {
    PyErr_Format(PyExc_Exception,
                 "Could not deserialize Python TaskStatus");
    return NULL;
  }

  int res = self->driver->sendStatusUpdate(status);
  return PyInt_FromLong(res); // Sets an exception if creating the int fails
}


PyObject* MesosExecutorDriverImpl_sendFrameworkMessage(
    MesosExecutorDriverImpl* self,
    PyObject* args)
{
  if (self->driver == NULL) {
    PyErr_Format(PyExc_Exception, "MesosExecutorDriverImpl.driver is NULL");
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
