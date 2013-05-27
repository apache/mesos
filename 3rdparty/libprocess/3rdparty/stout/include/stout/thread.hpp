#ifndef __STOUT_THREAD_HPP__
#define __STOUT_THREAD_HPP__

#include <pthread.h>
#include <stdio.h> // For perror.
#include <stdlib.h> // For abort.

template <typename T>
struct ThreadLocal
{
  ThreadLocal()
  {
    if (pthread_key_create(&key, NULL) != 0) {
      perror("Failed to create thread local, pthread_key_create");
      abort();
    }
  }

  ThreadLocal<T>& operator = (T* t)
  {
    if (pthread_setspecific(key, t) != 0) {
      perror("Failed to set thread local, pthread_setspecific");
      abort();
    }
    return *this;
  }

  operator T* () const
  {
    return reinterpret_cast<T*>(pthread_getspecific(key));
  }

  T* operator -> () const
  {
    return reinterpret_cast<T*>(pthread_getspecific(key));
  }

private:
  // Not expecting any other operators to be used (and the rest?).
  bool operator * (const ThreadLocal<T>&) const;
  bool operator == (const ThreadLocal<T>&) const;
  bool operator != (const ThreadLocal<T>&) const;
  bool operator < (const ThreadLocal<T>&) const;
  bool operator > (const ThreadLocal<T>&) const;

  pthread_key_t key;
};

#endif // __STOUT_THREAD_HPP__
