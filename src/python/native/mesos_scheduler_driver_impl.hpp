#ifndef MESOS_SCHEDULER_DRIVER_IMPL_HPP
#define MESOS_SCHEDULER_DRIVER_IMPL_HPP

#include "mesos_sched.hpp"

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

// MesosSchedulerDriverImpl methods
PyObject* MesosSchedulerDriverImpl_start(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_stop(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_join(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_run(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_reviveOffers(MesosSchedulerDriverImpl* self);

PyObject* MesosSchedulerDriverImpl_replyToOffer(MesosSchedulerDriverImpl* self,
                                                PyObject* args);

PyObject* MesosSchedulerDriverImpl_killTask(MesosSchedulerDriverImpl* self,
                                            PyObject* args);

PyObject* MesosSchedulerDriverImpl_sendFrameworkMessage(
    MesosSchedulerDriverImpl* self,
    PyObject* args);

}} /* namespace mesos { namespace python { */

#endif /* MESOS_SCHEDULER_DRIVER_IMPL_HPP */
