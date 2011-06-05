#ifndef LOCK_HPP
#define LOCK_HPP

#include <pthread.h>

namespace nexus { namespace internal {

/**
 * RAII class for locking pthread_mutexes.
 */
class Lock
{
  pthread_mutex_t* mutex;

public:
  Lock(pthread_mutex_t* _mutex);
  ~Lock();
};

}} /* namespace nexus { namespace internal { */

#endif /* LOCK_HPP */
