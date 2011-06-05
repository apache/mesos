#include <iostream>

#include "proxy_executor.hpp"
#include "module.hpp"
#include "mesos_executor_driver_impl.hpp"

using namespace mesos;

using std::cerr;
using std::endl;
using std::string;
using std::vector;
using std::map;


namespace mesos { namespace python {

void ProxyExecutor::init(ExecutorDriver* driver,
                         const ExecutorArgs& args)
{
  InterpreterLock lock;
  
  PyObject* argsObj = NULL;
  PyObject* res = NULL;
  
  argsObj = createPythonProtobuf(args, "ExecutorArgs");
  if (argsObj == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception
  }

  res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "init",
                            (char*) "OO",
                            impl,
                            argsObj);
  if (res == NULL) {
    cerr << "Failed to call executor's init" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->stop();
  }
  Py_XDECREF(argsObj);
  Py_XDECREF(res);
}


void ProxyExecutor::launchTask(ExecutorDriver* driver,
                               const TaskDescription& task)
{
  InterpreterLock lock;
  
  PyObject* taskObj = NULL;
  PyObject* res = NULL;
  
  taskObj = createPythonProtobuf(task, "TaskDescription");
  if (taskObj == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception
  }

  res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "launchTask",
                            (char*) "OO",
                            impl,
                            taskObj);
  if (res == NULL) {
    cerr << "Failed to call executor's launchTask" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->stop();
  }
  Py_XDECREF(taskObj);
  Py_XDECREF(res);
}


void ProxyExecutor::killTask(ExecutorDriver* driver,
                             const TaskID& taskId)
{
  InterpreterLock lock;
  
  PyObject* taskIdObj = NULL;
  PyObject* res = NULL;
  
  taskIdObj = createPythonProtobuf(taskId, "TaskID");
  if (taskIdObj == NULL) {
    goto cleanup; // createPythonProtobuf will have set an exception
  }

  res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "killTask",
                            (char*) "OO",
                            impl,
                            taskIdObj);
  if (res == NULL) {
    cerr << "Failed to call executor's killTask" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->stop();
  }
  Py_XDECREF(taskIdObj);
  Py_XDECREF(res);
}


void ProxyExecutor::frameworkMessage(ExecutorDriver* driver,
                                     const string& data)
{
  InterpreterLock lock;
  
  PyObject* res = NULL;
  
  res = PyObject_CallMethod(impl->pythonExecutor,
                            (char*) "frameworkMessage",
                            (char*) "Os",
                            impl,
                            data.c_str());
  if (res == NULL) {
    cerr << "Failed to call executor's frameworkMessage" << endl;
    goto cleanup;
  }

cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->stop();
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
  if (res == NULL) {
    cerr << "Failed to call executor's shutdown" << endl;
    goto cleanup;
  }
cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    driver->stop();
  }
  Py_XDECREF(res);
}


void ProxyExecutor::error(ExecutorDriver* driver,
                          int code,
                          const string& message)
{
  InterpreterLock lock;
  PyObject* res = PyObject_CallMethod(impl->pythonExecutor,
                                      (char*) "error",
                                      (char*) "Ois",
                                      impl,
                                      code,
                                      message.c_str());
  if (res == NULL) {
    cerr << "Failed to call executor's error" << endl;
    goto cleanup;
  }
cleanup:
  if (PyErr_Occurred()) {
    PyErr_Print();
    // No need for driver.stop(); it should stop itself
  }
  Py_XDECREF(res);
}

}} /* namespace mesos { namespace python { */
