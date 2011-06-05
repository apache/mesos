#ifdef __APPLE__
#define _XOPEN_SOURCE
#endif /* __APPLE__ */

#include <stdlib.h>

#include "tuple.hpp"

namespace process { namespace serialization {

void operator & (serializer &, const ::boost::tuples::null_type &)
{
  abort();
}

}}
