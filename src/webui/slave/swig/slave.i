%module(directors="1") slave

%{
#include <slave/state.hpp>

namespace mesos { namespace internal { namespace slave { namespace state {
extern SlaveState *get_slave();
}}}}

#define SWIG_STD_NOASSIGN_STL
%}

%import <stdint.i>
%import <std_string.i>
%import <std_vector.i>

%template(FrameworkVec) std::vector<mesos::internal::slave::state::Framework*>;
%template(TaskVec) std::vector<mesos::internal::slave::state::Task*>;

/* Rename task_state enum so that the generated class is called TaskState */
%rename(TaskState) task_state;

%include <mesos_types.h>
%include <mesos_types.hpp>

%include <slave/state.hpp>

namespace mesos { namespace internal { namespace slave { namespace state {
%newobject get_slave;
extern SlaveState *get_slave();
}}}}

