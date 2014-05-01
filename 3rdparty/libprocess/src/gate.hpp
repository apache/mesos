#ifndef __GATE_HPP__
#define __GATE_HPP__

// TODO(benh): Build implementation directly on-top-of futex's for Linux.

class Gate
{
public:
  typedef intptr_t state_t;

private:
  int waiters;
  state_t state;
  pthread_mutex_t mutex;
  pthread_cond_t cond;

public:
  Gate() : waiters(0), state(0)
  {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);
  }

  ~Gate()
  {
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mutex);
  }

  void open(bool all = true)
  {
    pthread_mutex_lock(&mutex);
    {
      state++;
      if (all) pthread_cond_broadcast(&cond);
      else pthread_cond_signal(&cond);
    }
    pthread_mutex_unlock(&mutex);
  }

  void wait()
  {
    pthread_mutex_lock(&mutex);
    {
      waiters++;
      state_t old = state;
      while (old == state) {
        pthread_cond_wait(&cond, &mutex);
      }
      waiters--;
    }
    pthread_mutex_unlock(&mutex);
  }

  state_t approach()
  {
    state_t old;
    pthread_mutex_lock(&mutex);
    {
      waiters++;
      old = state;
    }
    pthread_mutex_unlock(&mutex);
    return old;
  }

  void arrive(state_t old)
  {
    pthread_mutex_lock(&mutex);
    {
      while (old == state) {
        pthread_cond_wait(&cond, &mutex);
      }
      waiters--;
    }
    pthread_mutex_unlock(&mutex);
  }

  void leave()
  {
    pthread_mutex_lock(&mutex);
    {
      waiters--;
    }
    pthread_mutex_unlock(&mutex);
  }

  bool empty()
  {
    bool occupied = true;
    pthread_mutex_lock(&mutex);
    {
      occupied = waiters > 0 ? true : false;
    }
    pthread_mutex_unlock(&mutex);
    return !occupied;
  }
};

#endif // __GATE_HPP__
