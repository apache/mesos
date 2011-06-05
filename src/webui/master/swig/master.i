%module(directors="1") master

#define SWIG_NO_EXPORT_ITERATOR_METHODS

%{
#include <master/state.hpp>

namespace mesos { namespace internal { namespace master { namespace state {
extern MasterState *get_master();
}}}}

#define SWIG_STD_NOASSIGN_STL
%}

%include <stdint.i>
%include <std_string.i>
%include <std_vector.i>

%template(SlaveVec) std::vector<mesos::internal::master::state::Slave*>;
%template(FrameworkVec) std::vector<mesos::internal::master::state::Framework*>;
%template(TaskVec) std::vector<mesos::internal::master::state::Task*>;
%template(OfferVec) std::vector<mesos::internal::master::state::SlotOffer*>;
%template(SlaveResourcesVec) std::vector<mesos::internal::master::state::SlaveResources*>;

/* Rename task_state enum so that the generated class is called TaskState */
%rename(TaskState) task_state;

%include <mesos_types.h>
%include <mesos_types.hpp>

%include <master/state.hpp>

namespace mesos { namespace internal { namespace master { namespace state {
%newobject get_master;
extern MasterState *get_master();
}}}}

