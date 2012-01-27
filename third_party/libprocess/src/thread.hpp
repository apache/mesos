#ifndef __PROCESS_THREAD_HPP__
#define __PROCESS_THREAD_HPP__

#include <pthread.h>

#include <glog/logging.h>

template <typename T>
struct ThreadLocal
{
  explicit ThreadLocal(pthread_key_t _key) : key(_key) {}

  ThreadLocal<T>& operator = (T* t)
  {
    if (pthread_setspecific(key, t) != 0) {
      PLOG(FATAL) << "Failed to set thread local, pthread_setspecific";
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

  const pthread_key_t key;
};

#endif // __PROCESS_THREAD_HPP__
