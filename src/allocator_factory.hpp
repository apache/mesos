#ifndef __ALLOCATOR_FACTORY_HPP__
#define __ALLOCATOR_FACTORY_HPP__

#include "allocator.hpp"
#include "factory.hpp"

namespace mesos { namespace internal { namespace master {

DECLARE_FACTORY(Allocator, Master *);

}}}

#endif // __ALLOCATOR_FACTORY_HPP__
