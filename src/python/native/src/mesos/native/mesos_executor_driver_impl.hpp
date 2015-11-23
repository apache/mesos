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

#ifndef MESOS_EXECUTOR_DRIVER_IMPL_HPP
#define MESOS_EXECUTOR_DRIVER_IMPL_HPP

#include <mesos/executor.hpp>


namespace mesos { namespace python {

class ProxyExecutor;

/**
 * Python object structure for MesosExecutorDriverImpl objects.
 */
struct MesosExecutorDriverImpl {
    PyObject_HEAD
    /* Type-specific fields go here. */
    MesosExecutorDriver* driver;
    ProxyExecutor* proxyExecutor;
    PyObject* pythonExecutor;
};

/**
 * Python type object for MesosExecutorDriverImpl.
 */
extern PyTypeObject MesosExecutorDriverImplType;

/**
 * List of Python methods in MesosExecutorDriverImpl.
 */
extern PyMethodDef MesosExecutorDriverImpl_methods[];

/**
 * Create, but don't initialize, a new MesosExecutorDriverImpl
 * (called by Python before init method).
 */
PyObject* MesosExecutorDriverImpl_new(PyTypeObject *type,
                                      PyObject *args,
                                      PyObject *kwds);

/**
 * Initialize a MesosExecutorDriverImpl with constructor arguments.
 */
int MesosExecutorDriverImpl_init(MesosExecutorDriverImpl *self,
                                 PyObject *args,
                                 PyObject *kwds);

/**
 * Free a MesosExecutorDriverImpl.
 */
void MesosExecutorDriverImpl_dealloc(MesosExecutorDriverImpl* self);

/**
 * Traverse fields of a MesosExecutorDriverImpl on a cyclic GC search.
 * See http://docs.python.org/extending/newtypes.html.
 */
int MesosExecutorDriverImpl_traverse(MesosExecutorDriverImpl* self,
                                     visitproc visit,
                                     void* arg);
/**
 * Clear fields of a MesosExecutorDriverImpl that can participate in
 * GC cycles. See http://docs.python.org/extending/newtypes.html.
 */
int MesosExecutorDriverImpl_clear(MesosExecutorDriverImpl* self);

// MesosExecutorDriverImpl methods.
PyObject* MesosExecutorDriverImpl_start(MesosExecutorDriverImpl* self);

PyObject* MesosExecutorDriverImpl_stop(MesosExecutorDriverImpl* self);

PyObject* MesosExecutorDriverImpl_abort(MesosExecutorDriverImpl* self);

PyObject* MesosExecutorDriverImpl_join(MesosExecutorDriverImpl* self);

PyObject* MesosExecutorDriverImpl_run(MesosExecutorDriverImpl* self);

PyObject* MesosExecutorDriverImpl_sendStatusUpdate(
    MesosExecutorDriverImpl* self,
    PyObject* args);

PyObject* MesosExecutorDriverImpl_sendFrameworkMessage(
    MesosExecutorDriverImpl* self,
    PyObject* args);

} // namespace python {
} // namespace mesos {

#endif /* MESOS_EXECUTOR_DRIVER_IMPL_HPP */
