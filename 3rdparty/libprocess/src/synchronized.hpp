#ifndef __SYNCHRONIZABLE_HPP__
#define __SYNCHRONIZABLE_HPP__

#include <pthread.h>

#include <iostream>


class Synchronizable
{
public:
  Synchronizable()
    : initialized(false) {}

  explicit Synchronizable(int _type)
    : type(_type), initialized(false)
  {
    initialize();
  }

  Synchronizable(const Synchronizable &that)
  {
    type = that.type;
    initialize();
  }

  Synchronizable & operator = (const Synchronizable &that)
  {
    type = that.type;
    initialize();
    return *this;
  }

  void acquire()
  {
    if (!initialized) {
      ABORT("synchronizable not initialized");
    }
    pthread_mutex_lock(&mutex);
  }

  void release()
  {
    if (!initialized) {
      ABORT("synchronizable not initialized");
    }
    pthread_mutex_unlock(&mutex);
  }

private:
  void initialize()
  {
    if (!initialized) {
      pthread_mutexattr_t attr;
      pthread_mutexattr_init(&attr);
      pthread_mutexattr_settype(&attr, type);
      pthread_mutex_init(&mutex, &attr);
      pthread_mutexattr_destroy(&attr);
      initialized = true;
    } else {
      ABORT("synchronizable already initialized");
    }
  }

  int type;
  bool initialized;
  pthread_mutex_t mutex;
};


class Synchronized
{
public:
  explicit Synchronized(Synchronizable *_synchronizable)
    : synchronizable(_synchronizable)
  {
    synchronizable->acquire();
  }

  ~Synchronized()
  {
    synchronizable->release();
  }

  operator bool () { return true; }

private:
  Synchronizable *synchronizable;
};


#define synchronized(s)                                                 \
  if (Synchronized __synchronized ## s = Synchronized(&__synchronizable_ ## s))

#define synchronizable(s)                       \
  Synchronizable __synchronizable_ ## s

#define synchronizer(s)                         \
  (__synchronizable_ ## s)


#define SYNCHRONIZED_INITIALIZER                \
  Synchronizable(PTHREAD_MUTEX_NORMAL)

#define SYNCHRONIZED_INITIALIZER_DEBUG          \
  Synchronizable(PTHREAD_MUTEX_ERRORCHECK)

#define SYNCHRONIZED_INITIALIZER_RECURSIVE      \
  Synchronizable(PTHREAD_MUTEX_RECURSIVE)

#endif // __SYNCHRONIZABLE_HPP__
