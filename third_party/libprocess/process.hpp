#ifndef PROCESS_HPP
#define PROCESS_HPP

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

#include <sys/time.h>

#include <queue>

#include <tr1/functional>

#include "pid.hpp"


typedef uint16_t MSGID;


const MSGID PROCESS_ERROR = 1;
const MSGID PROCESS_TIMEOUT = 2;
const MSGID PROCESS_EXIT = 3;
const MSGID PROCESS_TERMINATE = 4;
const MSGID PROCESS_DISPATCH = 5;
const MSGID PROCESS_MSGID = PROCESS_DISPATCH + 1;


class Process;


struct msg
{
  PID from;
  PID to;
  MSGID id;
  uint32_t len;
};


class Clock {
public:
  static void pause();
  static void resume();
  static void advance(double secs);
};


class Filter {
public:
  virtual bool filter(msg *) = 0;
};


template <typename T>
struct Future
{
  Future();
  Future(const Future<T> &that);
  Future<T> & operator = (const Future<T> &that);
  virtual ~Future();
  void set(const T &t_);
  T get() const;

private:
  int *refs;
  T **t;
  Process *trigger;
};


template <typename T>
class Promise
{
public:
  Promise();
  Promise(const Promise<T> &that);
  virtual ~Promise();
  void set(const T &t_);
  void associate(const Future<T> &future_);

private:
  void operator = (const Promise<T> &);

  enum State {
    UNSET_UNASSOCIATED,
    SET_UNASSOCIATED,
    UNSET_ASSOCIATED,
    SET_ASSOCIATED,
  };

  int *refs;
  T **t;
  State *state;
  Future<T> **future;
};


template <>
class Promise<void>;


template <typename T>
class Promise<T&>;


template <typename T>
struct Result
{
  Result(const T &t_);
  Result(const Promise<T> &promise_);
  Result(const Result<T> &that);
  virtual ~Result();
  bool isPromise() const;
  Promise<T> getPromise() const;

  T get() const;

private:
  void operator = (const Result<T> &);

  int *refs;
  T *t;
  Promise<T> *promise;
};


template <>
struct Result<void>;


class Process {
public:
  Process();
  virtual ~Process();

  /* Returns pid of process; valid even before calling spawn. */
  PID self() const { return pid; }

protected:
  /* Function run when process spawned. */
  virtual void operator() ();

  /* Returns the sender's PID of the last dequeued (current) message. */
  PID from() const { return current != NULL ? current->from : PID(); }

  /* Returns the id of the last dequeued (current) message. */
  MSGID msgid() const { return current != NULL ? current->id : PROCESS_ERROR; }

  /* Returns pointer and length of body of last dequeued (current) message. */
  virtual const char * body(size_t *length) const;

  /* Put a message at front of queue (will not reschedule process). */
  virtual void inject(const PID &from, MSGID id, const char *data = NULL, size_t length = 0);

  /* Sends a message with data to PID. */
  virtual void send(const PID &to, MSGID id, const char *data = NULL, size_t length = 0);

  /* Blocks for message at most specified seconds (0 implies forever). */
  virtual MSGID receive(double secs = 0);

  /*  Processes dispatch messages. */
  virtual MSGID serve(double secs = 0, bool forever = true);

  /* Blocks at least specified seconds (may block longer). */
  virtual void pause(double secs);

  /* Links with the specified PID. */
  virtual PID link(const PID &pid);

  /* IO events for awaiting. */
  enum { RDONLY = 01, WRONLY = 02, RDWR = 03 };

  /* Wait until operation is ready for file descriptor (or message received if not ignored). */
  virtual bool await(int fd, int op, double secs = 0, bool ignore = true);

  /* Returns true if operation on file descriptor is ready. */
  virtual bool ready(int fd, int op);

  /* Returns sub-second elapsed time (according to this process). */
  double elapsed();

public:
  /**
   * Spawn a new process.
   *
   * @param process process to be spawned
   */
  static PID spawn(Process *process);

  /**
   * Wait for process to exit (returns true if actually waited on a process).
   *
   * @param PID id of the process
   */
  static bool wait(const PID &pid);

  /**
   * Invoke the thunk in a legacy safe way (i.e., outside of libprocess).
   *
   * @param thunk function to be invoked
   */
  static void invoke(const std::tr1::function<void (void)> &thunk);

  /**
   * Use the specified filter on messages that get enqueued (note,
   * however, that for now you cannot filter timeout messages).
   *
   * @param filter message filter
   */
  static void filter(Filter *filter);

  /**
   * Sends a message with data without a return address.
   *
   * @param to receiver
   * @param id message id
   * @param data data to send (gets copied)
   * @param length length of data
   */
  static void post(const PID &to, MSGID id, const char *data = NULL, size_t length = 0);

  /**
   * Dispatches a void method on a process.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   */
  template <typename C>
  static void dispatch(C *instance, void (C::*method)());

  /**
   * Dispatches a void method on a process.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 argument to pass to method
   */
  template <typename C, typename P1, typename A1>
  static void dispatch(C *instance, void (C::*method)(P1), A1 a1);

  /**
   * Dispatches a void method on a process.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   */
  template <typename C, typename P1, typename P2, typename A1, typename A2>
  static void dispatch(C *instance, void (C::*method)(P1, P2), A1 a1, A2 a2);

  /**
   * Dispatches a void method on a process.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 second argument to pass to method
   */
  template <typename C,
            typename P1, typename P2, typename P3,
            typename A1, typename A2, typename A3>
  static void dispatch(C *instance, void (C::*method)(P1, P2, P3),
                       A1 a1, A2 a2, A3 a3);

  /**
   * Dispatches a void method on a process.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   */
  template <typename C,
            typename P1, typename P2, typename P3, typename P4,
            typename A1, typename A2, typename A3, typename A4>
  static void dispatch(C *instance, void (C::*method)(P1, P2, P3, P4),
                       A1 a1, A2 a2, A3 a3, A4 a4);

  /**
   * Dispatches a void method on a process.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @param a5 fifth argument to pass to method
   */
  template <typename C,
            typename P1, typename P2, typename P3, typename P4, typename P5,
            typename A1, typename A2, typename A3, typename A4, typename A5>
  static void dispatch(C *instance, void (C::*method)(P1, P2, P3, P4, P5),
                       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @return future corresponding to the result of executing the method
   */
  template <typename T, typename C>
  static Future<T> dispatch(C *instance, Result<T> (C::*method)());

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename T, typename C, typename P1, typename A1>
  static Future<T> dispatch(C *instance, Result<T> (C::*method)(P1), A1 a1);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename T, typename C,
            typename P1, typename P2,
            typename A1, typename A2>
  static Future<T> dispatch(C *instance, Result<T> (C::*method)(P1, P2),
                            A1 a1, A2 a2);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 second argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename T, typename C,
            typename P1, typename P2, typename P3,
            typename A1, typename A2, typename A3>
  static Future<T> dispatch(C *instance, Result<T> (C::*method)(P1, P2, P3),
                            A1 a1, A2 a2, A3 a3);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename T, typename C,
            typename P1, typename P2, typename P3, typename P4,
            typename A1, typename A2, typename A3, typename A4>
  static Future<T> dispatch(C *instance, Result<T> (C::*method)(P1, P2, P3, P4),
                            A1 a1, A2 a2, A3 a3, A4 a4);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @param a5 fifth argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename T, typename C,
            typename P1, typename P2, typename P3, typename P4, typename P5,
            typename A1, typename A2, typename A3, typename A4, typename A5>
  static Future<T> dispatch(C *instance, Result<T> (C::*method)(P1, P2, P3, P4, P5),
                            A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @return result of executing the method
   */
  template <typename T, typename C>
  static T call(C *instance, Result<T> (C::*method)());

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 argument to pass to method
   * @return result of executing the method
   */
  template <typename T, typename C, typename P1, typename A1>
  static T call(C *instance, Result<T> (C::*method)(P1), A1 a1);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @return result of executing the method
   */
  template <typename T, typename C,
            typename P1, typename P2,
            typename A1, typename A2>
  static T call(C *instance, Result<T> (C::*method)(P1, P2), A1 a1, A2 a2);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 second argument to pass to method
   * @return result of executing the method
   */
  template <typename T, typename C,
            typename P1, typename P2, typename P3,
            typename A1, typename A2, typename A3>
  static T call(C *instance, Result<T> (C::*method)(P1, P2, P3),
                A1 a1, A2 a2, A3 a3);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @return result of executing the method
   */
  template <typename T, typename C,
            typename P1, typename P2, typename P3, typename P4,
            typename A1, typename A2, typename A3, typename A4>
  static T call(C *instance, Result<T> (C::*method)(P1, P2, P3, P4),
                A1 a1, A2 a2, A3 a3, A4 a4);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param instance running process to receive dispatch message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @param a5 fifth argument to pass to method
   * @return result of executing the method
   */
  template <typename T, typename C,
            typename P1, typename P2, typename P3, typename P4, typename P5,
            typename A1, typename A2, typename A3, typename A4, typename A5>
  static T call(C *instance, Result<T> (C::*method)(P1, P2, P3, P4, P5),
                A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

private:
  friend class LinkManager;
  friend class ProcessManager;
  friend class ProcessReference;
  friend void * schedule(void *);

  /* Flag indicating state of process. */
  enum { INIT,
	 READY,
	 RUNNING,
	 RECEIVING,
	 PAUSED,
	 AWAITING,
	 WAITING,
	 INTERRUPTED,
	 TIMEDOUT,
         EXITING,
	 EXITED } state;

  /* Active references. */
  int refs;

  /* Queue of received messages. */
  std::deque<msg *> msgs;

  /* Current message. */
  msg *current;

  /* Current "blocking" generation. */
  int generation;

  /* Process PID. */
  PID pid;

  /* Continuation/Context of process. */
  ucontext_t uctx;

  /* Lock/mutex protecting internals. */
  pthread_mutex_t m;
  void lock() { pthread_mutex_lock(&m); }
  void unlock() { pthread_mutex_unlock(&m); }

  /* Enqueues the specified message. */
  void enqueue(msg *msg);

  /* Dequeues a message or returns NULL. */
  msg * dequeue();

  /* Dispatches the delegator to the specified process. */
  static void dispatcher(Process *, std::tr1::function<void (void)> *delegator);
};


template <typename T>
void delegate(std::tr1::function<Result<T> (void)> *thunk, Future<T> *future)
{
  assert(thunk != NULL);
  assert(future != NULL);

  const Result<T> &result = (*thunk)();

  if (!result.isPromise()) {
    future->set(result.get());
  } else {
    result.getPromise().associate(*future);
  }

  delete thunk;
  delete future;
}


template <typename C>
void Process::dispatch(C *instance, void (C::*method)())
{
  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(method, instance));

  dispatcher(instance, delegator);
}


template <typename C, typename P1, typename A1>
void Process::dispatch(C *instance, void (C::*method)(P1), A1 a1)
{
  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(method, instance,
                                                       a1));

  dispatcher(instance, delegator);
}


template <typename C,
          typename P1, typename P2,
          typename A1, typename A2>
void Process::dispatch(C *instance, void (C::*method)(P1, P2), A1 a1, A2 a2)
{
  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(method, instance,
                                                       a1, a2));

  dispatcher(instance, delegator);
}


template <typename C,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
void Process::dispatch(C *instance, void (C::*method)(P1, P2, P3),
                       A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(method, instance,
                                                       a1, a2, a3));

  dispatcher(instance, delegator);
}


template <typename C,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
void Process::dispatch(C *instance, void (C::*method)(P1, P2, P3, P4),
              A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(method, instance, a1, a2, a3,
                                                 a4));

  dispatcher(instance, delegator);
}


template <typename C,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
void Process::dispatch(C *instance, void (C::*method)(P1, P2, P3, P4, P5),
                       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(method, instance,
                                                       a1, a2, a3, a4, a5));

  dispatcher(instance, delegator);
}


template <typename T, typename C>
Future<T> Process::dispatch(C *instance, Result<T> (C::*method)())
{
  std::tr1::function<Result<T> (void)> *thunk =
    new std::tr1::function<Result<T> (void)>(std::tr1::bind(method, instance));

  Future<T> *future = new Future<T>();

  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(&delegate<T>, thunk,
                                                       future));

  dispatcher(instance, delegator);

  return *future;
}


template <typename T, typename C, typename P1, typename A1>
Future<T> Process::dispatch(C *instance, Result<T> (C::*method)(P1), A1 a1)
{
  std::tr1::function<Result<T> (void)> *thunk =
    new std::tr1::function<Result<T> (void)>(std::tr1::bind(method, instance,
                                                            a1));

  Future<T> *future = new Future<T>();

  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(&delegate<T>, thunk,
                                                       future));

  dispatcher(instance, delegator);

  return *future;
}


template <typename T, typename C,
          typename P1, typename P2,
          typename A1, typename A2>
Future<T> Process::dispatch(C *instance, Result<T> (C::*method)(P1, P2),
                            A1 a1, A2 a2)
{
  std::tr1::function<Result<T> (void)> *thunk =
    new std::tr1::function<Result<T> (void)>(std::tr1::bind(method, instance,
                                                            a1, a2));

  Future<T> *future = new Future<T>();

  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(&delegate<T>, thunk,
                                                       future));

  dispatcher(instance, delegator);

  return *future;
}


template <typename T, typename C,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<T> Process::dispatch(C *instance, Result<T> (C::*method)(P1, P2, P3),
                            A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<Result<T> (void)> *thunk =
    new std::tr1::function<Result<T> (void)>(std::tr1::bind(method, instance,
                                                            a1, a2, a3));

  Future<T> *future = new Future<T>();

  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(&delegate<T>, thunk,
                                                       future));

  dispatcher(instance, delegator);

  return *future;
}


template <typename T, typename C,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<T> Process::dispatch(C *instance, Result<T> (C::*method)(P1, P2, P3, P4),
                            A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<Result<T> (void)> *thunk =
    new std::tr1::function<Result<T> (void)>(std::tr1::bind(method, instance,
                                                            a1, a2, a3, a4));

  Future<T> *future = new Future<T>();

  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(&delegate<T>, thunk,
                                                       future));

  dispatcher(instance, delegator);

  return *future;
}


template <typename T, typename C,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<T> Process::dispatch(C *instance, Result<T> (C::*method)(P1, P2, P3, P4, P5),
                            A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<Result<T> (void)> *thunk =
    new std::tr1::function<Result<T> (void)>(std::tr1::bind(method, instance,
                                                            a1, a2, a3, a4, a5));

  Future<T> *future = new Future<T>();

  std::tr1::function<void (void)> *delegator =
    new std::tr1::function<void (void)>(std::tr1::bind(&delegate<T>, thunk,
                                                       future));

  dispatcher(instance, delegator);

  return *future;
}


template <typename T, typename C>
T Process::call(C *instance, Result<T> (C::*method)())
{
  return dispatch(instance, method).get();
}


template <typename T, typename C, typename P1, typename A1>
T Process::call(C *instance, Result<T> (C::*method)(P1), A1 a1)
{
  return dispatch(instance, method, a1).get();
}


template <typename T, typename C,
          typename P1, typename P2,
          typename A1, typename A2>
T Process::call(C *instance, Result<T> (C::*method)(P1, P2), A1 a1, A2 a2)
{
  return dispatch(instance, method, a1, a2).get();
}


template <typename T, typename C,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
T Process::call(C *instance, Result<T> (C::*method)(P1, P2, P3),
                A1 a1, A2 a2, A3 a3)
{
  return dispatch(instance, method, a1, a2, a3).get();
}


template <typename T, typename C,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
T Process::call(C *instance, Result<T> (C::*method)(P1, P2, P3, P4),
                A1 a1, A2 a2, A3 a3, A4 a4)
{
  return dispatch(instance, method, a1, a2, a3, a4).get();
}


template <typename T, typename C,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
T Process::call(C *instance, Result<T> (C::*method)(P1, P2, P3, P4, P5),
                A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return dispatch(instance, method, a1, a2, a3, a4, a5).get();
}


template <typename T>
Future<T>::Future()
{
  refs = new int;
  *refs = 1;
  t = new T *;
  *t = NULL;
  trigger = new Process();
  Process::spawn(trigger);
}


template <typename T>
Future<T>::Future(const Future<T> &that)
{
  assert(that.refs > 0);
  __sync_fetch_and_add(that.refs, 1);
  refs = that.refs;
  t = that.t;
  trigger = that.trigger;
}


template <typename T>
Future<T> & Future<T>::operator = (const Future<T> &that)
{
  if (this != &that) {
    // Destructor ...
    assert(refs != NULL);
    if (__sync_sub_and_fetch(refs, 1) == 0) {
      delete refs;
      assert(t != NULL);
      if (*t != NULL)
        delete *t;
      assert(trigger != NULL);
      Process::post(trigger->self(), PROCESS_MSGID);
      Process::wait(trigger->self());
      delete trigger;
    }

    // Copy constructor ...
    assert(that.refs > 0);
    __sync_fetch_and_add(that.refs, 1);
    refs = that.refs;
    t = that.t;
    trigger = that.trigger;
  }
}


template <typename T>
Future<T>::~Future()
{
  assert(refs != NULL);
  if (__sync_sub_and_fetch(refs, 1) == 0) {
    delete refs;
    assert(t != NULL);
    if (*t != NULL)
      delete *t;
    assert(trigger != NULL);
    Process::post(trigger->self(), PROCESS_MSGID);
    Process::wait(trigger->self());
    delete trigger;
  }
}


template <typename T>
void Future<T>::set(const T &t_)
{
  assert(t != NULL && *t == NULL);
  *t = new T(t_);
  Process::post(trigger->self(), PROCESS_MSGID);
}


template <typename T>
T Future<T>::get() const
{
  assert(t != NULL);
  if (*t != NULL)
    return **t;
  assert(trigger != NULL);
  Process::wait(trigger->self());
  assert(t != NULL && *t != NULL);
  return **t;
}


// TODO(benh): Use synchronized instead of CAS?
#define CAS __sync_bool_compare_and_swap


template <typename T>
Promise<T>::Promise()
{
  refs = new int;
  *refs = 1;
  t = new T *;
  *t = NULL;
  state = new State;
  *state = UNSET_UNASSOCIATED;
  future = new Future<T> *;
  *future = NULL;
}


template <typename T>
Promise<T>::Promise(const Promise<T> &that)
{
  assert(that.refs > 0);
  __sync_fetch_and_add(that.refs, 1);
  refs = that.refs;
  t = that.t;
  state = that.state;
  future = that.future;
}


template <typename T>
Promise<T>::~Promise()
{
  assert(refs != NULL);
  if (__sync_sub_and_fetch(refs, 1) == 0) {
    delete refs;
    assert(t != NULL);
    if (*t != NULL)
      delete *t;
    assert(state != NULL);
    delete state;
    assert(future != NULL);
    if (*future != NULL)
      delete *future;
  }
}


template <typename T>
void Promise<T>::set(const T &t_)
{
  assert(state != NULL);
  assert(*state == UNSET_UNASSOCIATED ||
         *state == UNSET_ASSOCIATED);
  assert(t != NULL && *t == NULL);
  if (*state == UNSET_UNASSOCIATED) {
    *t = new T(t_);
    if (!__sync_bool_compare_and_swap(state, UNSET_UNASSOCIATED, SET_UNASSOCIATED)) {
      assert(*state == UNSET_ASSOCIATED);
      __sync_bool_compare_and_swap(state, UNSET_ASSOCIATED, SET_ASSOCIATED);
      assert(future != NULL && *future != NULL);
      (*future)->set(**t);
    }
  } else {
    assert(*state == UNSET_ASSOCIATED);
    assert(future != NULL && *future != NULL);
    (*future)->set(t_);
    __sync_bool_compare_and_swap(state, UNSET_ASSOCIATED, SET_ASSOCIATED);
  }
}


template <typename T>
void Promise<T>::associate(const Future<T> &future_)
{
  assert(state != NULL);
  assert(*state == UNSET_UNASSOCIATED ||
         *state == SET_UNASSOCIATED);
  assert(future != NULL);
  *future = new Future<T>(future_);
  if (*state == UNSET_UNASSOCIATED) {
    if (!__sync_bool_compare_and_swap(state, UNSET_UNASSOCIATED,
                                      UNSET_ASSOCIATED)) {
      assert(*state == SET_UNASSOCIATED);
      __sync_bool_compare_and_swap(state, SET_UNASSOCIATED, SET_ASSOCIATED);
      assert(*state == SET_ASSOCIATED);
      assert(t != NULL && *t != NULL);
      (*future)->set(**t);
    }
  } else {
    assert(*state == SET_UNASSOCIATED);
    __sync_bool_compare_and_swap(state, SET_UNASSOCIATED, SET_ASSOCIATED);
    assert(*state == SET_ASSOCIATED);
    assert(t != NULL && *t != NULL);
    (*future)->set(**t);
  }
}


template <typename T>
Result<T>::Result(const T &t_)
{
  refs = new int;
  *refs = 1;
  t = new T(t_);
  promise = NULL;
}


template <typename T>
Result<T>::Result(const Promise<T> &promise_)
{
  refs = new int;
  *refs = 1;
  t = NULL;
  promise = new Promise<T>(promise_);
}


template <typename T>
Result<T>::Result(const Result<T> &that)
{
  assert(that.refs > 0);
  __sync_fetch_and_add(that.refs, 1);
  refs = that.refs;
  t = that.t;
  promise = that.promise;
}


template <typename T>
Result<T>::~Result()
{
  assert(refs != NULL);
  if (__sync_sub_and_fetch(refs, 1) == 0) {
    delete refs;
    if (t != NULL)
      delete t;
    if (promise != NULL)
      delete promise;
  }
}


template <typename T>
bool Result<T>::isPromise() const
{
  return promise != NULL;
}


template <typename T>
Promise<T> Result<T>::getPromise() const
{
  assert(isPromise());
  return *promise;
}


template <typename T>
T Result<T>::get() const
{
  assert(!isPromise());
  return *t;
}


#endif /* PROCESS_HPP */
