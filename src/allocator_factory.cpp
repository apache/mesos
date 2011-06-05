#include "allocator_factory.hpp"
#include "simple_allocator.hpp"

using namespace nexus::internal::master;

DEFINE_FACTORY(Allocator, Master *)
{
  registerClass<SimpleAllocator>("simple");
}
