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

#ifndef MESOS_SCHEDULER_DRIVER_IMPL_HPP
#define MESOS_SCHEDULER_DRIVER_IMPL_HPP

#include <mesos/scheduler.hpp>


namespace mesos { namespace python {

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
 * Python type object for MesosSchedulerDriverImpl.
 */
extern PyTypeObject MesosSchedulerDriverImplType;

/**
 * List of Python methods in MesosSchedulerDriverImpl.
 */
extern PyMethodDef MesosSchedulerDriverImpl_methods[];

/**
 * Create, but don't initialize, a new MesosSchedulerDriverImpl
 * (called by Python before init method).
 */
PyObject* MesosSchedulerDriverImpl_new(PyTypeObject *type,
                                       PyObject *args,
                                       PyObject *kwds);

/**
 * Initialize a MesosSchedulerDriverImpl with constructor arguments.
 */
int MesosSchedulerDriverImpl_init(MesosSchedulerDriverImpl *self,
                                  PyObject *args,
                                  PyObject *kwds);

/**
 * Free a MesosSchedulerDriverImpl.
 */
void MesosSchedulerDriverImpl_dealloc(MesosSchedulerDriverImpl* self);

/**
 * Traverse fields of a MesosSchedulerDriverImpl on a cyclic GC search.
 * See http://docs.python.org/extending/newtypes.html.
 */
int MesosSchedulerDriverImpl_traverse(MesosSchedulerDriverImpl* self,
                                      visitproc visit,
                                      void* arg);
/**
 * Clear fields of a MesosSchedulerDriverImpl that can participate in
 * GC cycles. See http://docs.python.org/extending/newtypes.html.
 */
int MesosSchedulerDriverImpl_clear(MesosSchedulerDriverImpl* self);

// MesosSchedulerDriverImpl methods.
PyObject* MesosSchedulerDriverImpl_start(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_stop(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

PyObject* MesosSchedulerDriverImpl_abort(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_join(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_run(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_requestResources(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

PyObject* MesosSchedulerDriverImpl_launchTasks(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

PyObject* MesosSchedulerDriverImpl_killTask(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

PyObject* MesosSchedulerDriverImpl_acceptOffers(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

PyObject* MesosSchedulerDriverImpl_declineOffer(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

PyObject* MesosSchedulerDriverImpl_reviveOffers(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_suppressOffers(
    MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_acknowledgeStatusUpdate(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

PyObject* MesosSchedulerDriverImpl_sendFrameworkMessage(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

PyObject* MesosSchedulerDriverImpl_reconcileTasks(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

} // namespace python {
} // namespace mesos {

#endif /* MESOS_SCHEDULER_DRIVER_IMPL_HPP */
