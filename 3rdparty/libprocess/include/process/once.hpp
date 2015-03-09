#ifndef __PROCESS_ONCE_HPP__
#define __PROCESS_ONCE_HPP__

#include <process/future.hpp>

#include <stout/nothing.hpp>

namespace process {

// Provides a _blocking_ abstraction that's useful for performing a
// task exactly once.
class Once
{
public:
  Once() : started(false), finished(false)
  {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);
  }

  ~Once()
  {
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mutex);
  }

  // Returns true if this Once instance has already transitioned to a
  // 'done' state (i.e., the action you wanted to perform "once" has
  // been completed). Note that this BLOCKS until Once::done has been
  // called.
  bool once()
  {
    bool result = false;

    pthread_mutex_lock(&mutex);
    {
      if (started) {
        while (!finished) {
          pthread_cond_wait(&cond, &mutex);
        }
        result = true;
      } else {
        started = true;
      }
    }
    pthread_mutex_unlock(&mutex);

    return result;
  }

  // Transitions this Once instance to a 'done' state.
  void done()
  {
    pthread_mutex_lock(&mutex);
    {
      if (started && !finished) {
        finished = true;
        pthread_cond_broadcast(&cond);
      }
    }
    pthread_mutex_unlock(&mutex);
  }

private:
  // Not copyable, not assignable.
  Once(const Once& that);
  Once& operator = (const Once& that);

  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool started;
  bool finished;
};

}  // namespace process {

#endif // __PROCESS_ONCE_HPP__
