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

%include <slave/state.hpp>

namespace mesos { namespace internal { namespace slave { namespace state {
%newobject get_slave;
extern SlaveState *get_slave();
}}}}

