#ifndef __ISOLATION_MODULE_FACTORY_HPP__
#define __ISOLATION_MODULE_FACTORY_HPP__

#include "isolation_module.hpp"
#include "factory.hpp"

namespace mesos { namespace internal { namespace slave {

DECLARE_FACTORY(IsolationModule, Slave *);

}}}

#endif // __ISOLATION_MODULE_FACTORY_HPP__
