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

#include "mesos_sched.hpp"
#include "mesos_exec.hpp"

#include "module.hpp"
#include "proxy_scheduler.hpp"
#include "mesos_scheduler_driver_impl.hpp"

using std::cout;
using std::cerr;
using std::endl;
using std::string;
using std::vector;
using std::map;
using namespace mesos;
using namespace mesos::python;


/**
 * The Python module object for mesos_pb2 (which contains the protobuf
 * classes generated for Python).
 */
PyObject* mesos::python::mesos_pb2 = NULL;


namespace {

/**
 * Method list for our Python module.
 */
PyMethodDef MODULE_METHODS[] = {
  {NULL, NULL, 0, NULL}        /* Sentinel */
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
