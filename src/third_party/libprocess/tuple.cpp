#include <stdlib.h>

#include "tuple.hpp"

namespace process { namespace serialization {

void operator & (serializer &, const ::boost::tuples::null_type &)
{
  abort();
}

}}
