#include "lock.hpp"

using namespace mesos::internal;


Lock::Lock(pthread_mutex_t* _mutex): mutex(_mutex)
{
  pthread_mutex_lock(mutex);
}


Lock::~Lock()
{
  pthread_mutex_unlock(mutex);
}
