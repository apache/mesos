#include <mesos.h>

#include "configuration.hpp"


using namespace mesos;
using namespace mesos::internal;


int params_get_int(const char *params, const char *key, int defVal)
{
  return Params(params).getInt(key, defVal);
}


int32_t params_get_int32(const char *params, const char *key, int32_t defVal)
{
  return Params(params).getInt32(key, defVal);
}


int64_t params_get_int64(const char *params, const char *key, int64_t defVal)
{
  return Params(params).getInt64(key, defVal);
}
