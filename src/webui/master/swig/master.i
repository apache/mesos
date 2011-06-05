%module(directors="1") master

#define SWIG_NO_EXPORT_ITERATOR_METHODS

%{
#include <master_state.hpp>

namespace nexus { namespace internal { namespace master { namespace state {
extern MasterState *get_master();
}}}}

#define SWIG_STD_NOASSIGN_STL
%}

%include <stdint.i>
%include <std_string.i>
%include <std_vector.i>

%template(SlaveVec) std::vector<nexus::internal::master::state::Slave*>;
%template(FrameworkVec) std::vector<nexus::internal::master::state::Framework*>;
%template(TaskVec) std::vector<nexus::internal::master::state::Task*>;
%template(OfferVec) std::vector<nexus::internal::master::state::SlotOffer*>;
%template(SlaveResourcesVec) std::vector<nexus::internal::master::state::SlaveResources*>;

/* Rename task_state enum so that the generated class is called TaskState */
%rename(TaskState) task_state;

%include <nexus_types.h>
%include <nexus_types.hpp>
%include <master_state.hpp>

namespace nexus { namespace internal { namespace master { namespace state {
%newobject get_master;
extern MasterState *get_master();
}}}}

