#ifndef __PROCESS_HPP__
#define __PROCESS_HPP__

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

#include <sys/time.h>

#include <queue>

#include <tr1/functional>

#include "future.hpp"
#include "pid.hpp"
#include "promise.hpp"


const std::string ERROR = "error";
const std::string TIMEOUT = "timeout";
const std::string EXIT = "exit";
const std::string TERMINATE = "terminate";


struct Message {
  std::string name;
  UPID from;
  UPID to;
  std::string body;
};


class Clock {
public:
  static void pause();
  static void resume();
  static void advance(double secs);
};


class Filter {
public:
  virtual bool filter(Message*) = 0;
};


class Process {
public:
  Process(const std::string& id = "");

  virtual ~Process();

  /* Returns pid of process; valid even before calling spawn. */
  UPID self() const { return pid; }

protected:
  /* Function run when process spawned. */
  virtual void operator() ();

  /* Returns the sender's PID of the last dequeued (current) message. */
  UPID from() const;

  /* Returns the name of the last dequeued (current) message. */
  const std::string& name() const;

  /* Returns pointer and length of body of last dequeued (current) message. */
  const char* body(size_t* length) const;

  /* Put a message at front of queue (will not reschedule process). */
  void inject(const UPID& from, const std::string& name, const char* data = NULL, size_t length = 0);

  /* Sends a message with data to PID. */
  void send(const UPID& to, const std::string &name, const char *data = NULL, size_t length = 0);

  /* Blocks for message at most specified seconds (0 implies forever). */
  std::string receive(double secs = 0);

  /*  Processes dispatch messages. */
  std::string serve(double secs = 0, bool forever = true);

  /* Blocks at least specified seconds (may block longer). */
  void pause(double secs);

  /* Links with the specified PID. */
  UPID link(const UPID& pid);

  /* IO events for awaiting. */
  enum { RDONLY = 01, WRONLY = 02, RDWR = 03 };

  /* Wait until operation is ready for file descriptor (or message received if not ignored). */
  bool await(int fd, int op, double secs = 0, bool ignore = true);

  /* Returns true if operation on file descriptor is ready. */
  bool ready(int fd, int op);

  /* Returns sub-second elapsed time (according to this process). */
  double elapsed();

public:
  /**
   * Spawn a new process.
   *
   * @param process process to be spawned
   */
  static UPID spawn(Process* process);

  /**
   * Wait for process to exit (returns true if actually waited on a process).
   *
   * @param PID id of the process
   */
  static bool wait(const UPID& pid);

  /**
   * Invoke the thunk in a legacy safe way (i.e., outside of libprocess).
   *
   * @param thunk function to be invoked
   */
  static void invoke(const std::tr1::function<void(void)>& thunk);

  /**
   * Use the specified filter on messages that get enqueued (note,
   * however, that for now you cannot filter timeout messages).
   *
   * @param filter message filter
   */
  static void filter(Filter* filter);

  /**
   * Sends a message with data without a return address.
   *
   * @param to receiver
   * @param name message name
   * @param data data to send (gets copied)
   * @param length length of data
   */
  static void post(const UPID& to, const std::string& name, const char* data = NULL, size_t length = 0);

  /**
   * Dispatches a void method on a process.
   *
   * @param pid receiver of message
   * @param method method to invoke on receiver
   */
  template <typename T>
  static void dispatch(const PID<T>& pid, void (T::*method)());

  /**
   * Dispatches a void method on a process.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 argument to pass to method
   */
  template <typename T, typename P1, typename A1>
  static void dispatch(const PID<T>& pid, void (T::*method)(P1), A1 a1);

  /**
   * Dispatches a void method on a process.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   */
  template <typename T, typename P1, typename P2, typename A1, typename A2>
  static void dispatch(const PID<T>& pid, void (T::*method)(P1, P2), A1 a1, A2 a2);

  /**
   * Dispatches a void method on a process.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 second argument to pass to method
   */
  template <typename T,
            typename P1, typename P2, typename P3,
            typename A1, typename A2, typename A3>
  static void dispatch(const PID<T>& pid, void (T::*method)(P1, P2, P3),
                       A1 a1, A2 a2, A3 a3);

  /**
   * Dispatches a void method on a process.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   */
  template <typename T,
            typename P1, typename P2, typename P3, typename P4,
            typename A1, typename A2, typename A3, typename A4>
  static void dispatch(const PID<T>& pid, void (T::*method)(P1, P2, P3, P4),
                       A1 a1, A2 a2, A3 a3, A4 a4);

  /**
   * Dispatches a void method on a process.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @param a5 fifth argument to pass to method
   */
  template <typename T,
            typename P1, typename P2, typename P3, typename P4, typename P5,
            typename A1, typename A2, typename A3, typename A4, typename A5>
  static void dispatch(const PID<T>& pid, void (T::*method)(P1, P2, P3, P4, P5),
                       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @return future corresponding to the result of executing the method
   */
  template <typename R, typename T>
  static Future<R> dispatch(const PID<T>& pid, Promise<R> (T::*method)());

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename R, typename T, typename P1, typename A1>
  static Future<R> dispatch(const PID<T>& pid,
                            Promise<R> (T::*method)(P1),
                            A1 a1);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename R, typename T,
            typename P1, typename P2,
            typename A1, typename A2>
  static Future<R> dispatch(const PID<T>& pid,
                            Promise<R> (T::*method)(P1, P2),
                            A1 a1, A2 a2);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 second argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename R, typename T,
            typename P1, typename P2, typename P3,
            typename A1, typename A2, typename A3>
  static Future<R> dispatch(const PID<T>& pid,
                            Promise<R> (T::*method)(P1, P2, P3),
                            A1 a1, A2 a2, A3 a3);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename R, typename T,
            typename P1, typename P2, typename P3, typename P4,
            typename A1, typename A2, typename A3, typename A4>
  static Future<R> dispatch(const PID<T>& pid,
                            Promise<R> (T::*method)(P1, P2, P3, P4),
                            A1 a1, A2 a2, A3 a3, A4 a4);

  /**
   * Dispatches a method on a process and returns the future that
   * corresponds to the result of executing the method.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @param a5 fifth argument to pass to method
   * @return future corresponding to the result of executing the method
   */
  template <typename R, typename T,
            typename P1, typename P2, typename P3, typename P4, typename P5,
            typename A1, typename A2, typename A3, typename A4, typename A5>
  static Future<R> dispatch(const PID<T>& pid,
                            Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                            A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @return result of executing the method
   */
  template <typename R, typename T>
  static R call(const PID<T>& pid, Promise<R> (T::*method)());

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 argument to pass to method
   * @return result of executing the method
   */
  template <typename R, typename T, typename P1, typename A1>
  static R call(const PID<T>& pid, Promise<R> (T::*method)(P1), A1 a1);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @return result of executing the method
   */
  template <typename R, typename T,
            typename P1, typename P2,
            typename A1, typename A2>
  static R call(const PID<T>& pid,
                Promise<R> (T::*method)(P1, P2),
                A1 a1, A2 a2);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 second argument to pass to method
   * @return result of executing the method
   */
  template <typename R, typename T,
            typename P1, typename P2, typename P3,
            typename A1, typename A2, typename A3>
  static R call(const PID<T>& pid,
                Promise<R> (T::*method)(P1, P2, P3),
                A1 a1, A2 a2, A3 a3);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @return result of executing the method
   */
  template <typename R, typename T,
            typename P1, typename P2, typename P3, typename P4,
            typename A1, typename A2, typename A3, typename A4>
  static R call(const PID<T>& pid,
                Promise<R> (T::*method)(P1, P2, P3, P4),
                A1 a1, A2 a2, A3 a3, A4 a4);

  /**
   * Dispatches a method on a process and waits (on the underlying
   * future) for the result.
   *
   * @param pid receiver of message
   * @param method method to invoke on instance
   * @param a1 first argument to pass to method
   * @param a2 second argument to pass to method
   * @param a3 third argument to pass to method
   * @param a4 fourth argument to pass to method
   * @param a5 fifth argument to pass to method
   * @return result of executing the method
   */
  template <typename R, typename T,
            typename P1, typename P2, typename P3, typename P4, typename P5,
            typename A1, typename A2, typename A3, typename A4, typename A5>
  static R call(const PID<T>& pid,
                Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

private:
  friend class LinkManager;
  friend class ProcessManager;
  friend class ProcessReference;
  friend void* schedule(void *);

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
  std::deque<Message*> messages;

  /* Current message. */
  Message* current;

  /* Current "blocking" generation. */
  int generation;

  /* Process PID. */
  UPID pid;

  /* Continuation/Context of process. */
  ucontext_t uctx;

  /* Lock/mutex protecting internals. */
  pthread_mutex_t m;
  void lock() { pthread_mutex_lock(&m); }
  void unlock() { pthread_mutex_unlock(&m); }

  /* Enqueues the specified message. */
  void enqueue(Message* message);

  /* Dequeues a message or returns NULL. */
  Message* dequeue();

  template <typename T>
  static void vdelegate(Process* process, std::tr1::function<void(T*)>* thunk)
  {
    assert(process != NULL);
    assert(thunk != NULL);
    (*thunk)(static_cast<T*>(process));
    delete thunk;
  }

  template <typename R, typename T>
  static void delegate(Process* process, std::tr1::function<Promise<R>(T*)>* thunk, Future<R>* future)
  {
    assert(process != NULL);
    assert(thunk != NULL);
    assert(future != NULL);
    (*thunk)(static_cast<T*>(process)).associate(future);
    delete thunk;
  }

  /* Dispatches the delegator to the specified process. */
  static void dispatcher(const UPID& pid, std::tr1::function<void(Process*)>* delegator);
};


template <typename T>
void Process::dispatch(const PID<T>& pid, void (T::*method)())
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1));

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::vdelegate<T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk));

  dispatcher(pid, delegator);
}


template <typename T, typename P1, typename A1>
void Process::dispatch(const PID<T>& pid, void (T::*method)(P1), A1 a1)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1));

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::vdelegate<T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk));

  dispatcher(pid, delegator);
}


template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
void Process::dispatch(const PID<T>& pid, void (T::*method)(P1, P2), A1 a1, A2 a2)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1, a2));

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::vdelegate<T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk));

  dispatcher(pid, delegator);
}


template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
void Process::dispatch(const PID<T>& pid, void (T::*method)(P1, P2, P3),
                       A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1, a2, a3));

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::vdelegate<T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk));

  dispatcher(pid, delegator);
}


template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
void Process::dispatch(const PID<T>& pid, void (T::*method)(P1, P2, P3, P4),
                       A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1, a2, a3, a4));

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::vdelegate<T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk));

  dispatcher(pid, delegator);
}


template <typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
void Process::dispatch(const PID<T>& pid, void (T::*method)(P1, P2, P3, P4, P5),
                       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1, a2, a3, a4, a5));

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::vdelegate<T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk));

  dispatcher(pid, delegator);
}


template <typename R, typename T>
Future<R> Process::dispatch(const PID<T>& pid, Promise<R> (T::*method)())
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::delegate<R, T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


template <typename R, typename T, typename P1, typename A1>
Future<R> Process::dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1), A1 a1)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::delegate<R, T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> Process::dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1, P2),
                            A1 a1, A2 a2)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1, a2));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::delegate<R, T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> Process::dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3),
                            A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1, a2, a3));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::delegate<R, T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> Process::dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3, P4),
                            A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1, a2, a3, a4));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::delegate<R, T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> Process::dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                            A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1, a2, a3, a4, a5));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(Process*)>* delegator =
    new std::tr1::function<void(Process*)>(std::tr1::bind(&Process::delegate<R, T>,
                                                          std::tr1::placeholders::_1,
                                                          thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


template <typename R, typename T>
R Process::call(const PID<T>& pid, Promise<R> (T::*method)())
{
  return dispatch(pid, method).get();
}


template <typename R, typename T, typename P1, typename A1>
R Process::call(const PID<T>& pid, Promise<R> (T::*method)(P1), A1 a1)
{
  return dispatch(pid, method, a1).get();
}


template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R Process::call(const PID<T>& pid, Promise<R> (T::*method)(P1, P2), A1 a1, A2 a2)
{
  return dispatch(pid, method, a1, a2).get();
}


template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R Process::call(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3),
                A1 a1, A2 a2, A3 a3)
{
  return dispatch(pid, method, a1, a2, a3).get();
}


template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R Process::call(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3, P4),
                A1 a1, A2 a2, A3 a3, A4 a4)
{
  return dispatch(pid, method, a1, a2, a3, a4).get();
}


template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R Process::call(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return dispatch(pid, method, a1, a2, a3, a4, a5).get();
}

#endif // __PROCESS_HPP__
