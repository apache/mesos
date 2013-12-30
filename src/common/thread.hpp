#ifndef __THREAD_HPP__
#define __THREAD_HPP__

#include <pthread.h>

#include <stout/lambda.hpp>

// Provides a simple threading facility for starting a thread to run
// an arbitrary function. No mechanism for returning a value from the
// function is currently provided (and in the future would probably be
// provided by libprocess anyway).

namespace thread {

void* __run(void* arg)
{
  lambda::function<void(void)>* function =
    reinterpret_cast<lambda::function<void(void)>*>(arg);
  (*function)();
  delete function;
  return 0;
}


bool start(const lambda::function<void(void)>& f, bool detach = false)
{
  lambda::function<void(void)>* __f = new lambda::function<void(void)>(f);

  pthread_t t;
  if (pthread_create(&t, NULL, __run, __f) != 0) {
    return false;
  }

  if (detach && pthread_detach(t) != 0) {
    return false;
  }

  return true;
}

// TODO(benh): Provide a version of 'run' that returns a type T (the
// value being a copy or preferablly via move semantics).

} // namespace thread {

#endif // __THREAD_HPP__
