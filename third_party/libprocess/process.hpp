#ifndef __PROCESS_HPP__
#define __PROCESS_HPP__

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

#include <sys/time.h>

#include <map>
#include <queue>

#include <tr1/functional>

#include "future.hpp"
#include "http.hpp"
#include "pid.hpp"
#include "promise.hpp"


namespace process {

const std::string ERROR = "__process_error__";
const std::string TIMEOUT = "__process_timeout__";
const std::string EXITED = "__process_exited__";
const std::string TERMINATE = "__process_terminate__";


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
  // TODO(benh): Support filtering HTTP requests?
  virtual bool filter(Message*) = 0;
};


class ProcessBase
{
public:
  ProcessBase(const std::string& id = "");

  virtual ~ProcessBase();

  UPID self() const { return pid; }

  static UPID spawn(ProcessBase* process);

protected:
  /* Function run when process spawned. */
  virtual void operator () () = 0;

  /* Returns the sender's PID of the last dequeued (current) message. */
  UPID from() const;

  /* Returns the name of the last dequeued (current) message. */
  const std::string& name() const;

  /* Returns pointer and length of body of last dequeued (current) message. */
  const std::string& body() const;

  /* Put a message at front of queue (will not reschedule process). */
  void inject(const UPID& from, const std::string& name, const char* data = NULL, size_t length = 0);

  /* Sends a message with data to PID. */
  void send(const UPID& to, const std::string &name, const char *data = NULL, size_t length = 0);

  /* Blocks for message at most specified seconds (0 implies forever). */
  std::string receive(double secs = 0);

  /*  Processes dispatch messages. */
  std::string serve(double secs = 0, bool once = false);

  /* Blocks at least specified seconds (may block longer). */
  void pause(double secs);

  /* Links with the specified PID. */
  UPID link(const UPID& pid);

  /* IO events for polling. */
  enum { RDONLY = 01, WRONLY = 02, RDWR = 03 };

  /* Wait until operation is ready for file descriptor (or message received if not ignored). */
  bool poll(int fd, int op, double secs = 0, bool ignore = true);

  /* Returns true if operation on file descriptor is ready. */
  bool ready(int fd, int op);

  /* Returns sub-second elapsed time (according to this process). */
  double elapsed();

  /* Install a handler for a message. */
  template <typename T>
  void install(const std::string& name, void (T::*method)())
  {
    message_handlers[name] =
      std::tr1::bind(method, static_cast<T*>(this));
  }

  /* Install a handler for an HTTP request. */
  template <typename T>
  void install(const std::string& name,
               Promise<HttpResponse> (T::*method)(const HttpRequest&))
  {
    http_handlers[name] =
      std::tr1::bind(method, static_cast<T*>(this), std::tr1::placeholders::_1);
  }

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
	 SERVING,
	 PAUSED,
	 POLLING,
	 WAITING,
	 INTERRUPTED,
	 TIMEDOUT,
         FINISHING,
	 FINISHED } state;

  /* Lock/mutex protecting internals. */
  pthread_mutex_t m;
  void lock() { pthread_mutex_lock(&m); }
  void unlock() { pthread_mutex_unlock(&m); }

  /* Enqueue the specified message, request, or delegator. */
  void enqueue(Message* message);
  void enqueue(std::pair<HttpRequest*, Future<HttpResponse>*>* request);
  void enqueue(std::tr1::function<void(ProcessBase*)>* delegator);

  /* Dequeue a message, request, or delegator, or returns NULL. */
  template <typename T>
  T* dequeue();

  /* Queue of received messages. */
  std::deque<Message*> messages;

  /* Queue of HTTP requests (with associated futures for responses). */
  std::deque<std::pair<HttpRequest*, Future<HttpResponse>*>*> requests;

  /* Queue of dispatches. */
  std::deque<std::tr1::function<void(ProcessBase*)>*> delegators;

  /* Handlers for messages. */
  std::map<std::string, std::tr1::function<void(void)> > message_handlers;

  /* Handlers for HTTP requests. */
  std::map<std::string, std::tr1::function<Promise<HttpResponse>(const HttpRequest&)> > http_handlers;

  /* Current message. */
  Message* current;

  /* Active references. */
  int refs;

  /* Current "blocking" generation. */
  int generation;

  /* Process PID. */
  UPID pid;

  /* Continuation/Context of process. */
  ucontext_t uctx;
};


template <typename T>
class Process : public ProcessBase {
public:
  Process(const std::string& id = "") : ProcessBase(id) {}

  /* Returns pid of process; valid even before calling spawn. */
  PID<T> self() const { return PID<T>(static_cast<const T&>(*this)); }

protected:
  virtual void operator () ()
  {
    while (true) serve();
  }
};


/**
 * Initialize the library.
 *
 * @param initGoogleLogging whether or not to initialize the Google
 *        Logging library (glog). If the application is also using
 *        glog, this should be set to false.
 */
void initialize(bool initialize_google_logging = true);


/**
 * Spawn a new process.
 *
 * @param process process to be spawned
 */
template <typename T>
PID<T> spawn(Process<T>* process)
{
  if (!ProcessBase::spawn(process)) {
    return PID<T>();
  }

  return process->self();
}


/**
 * Wait for process to exit (returns true if actually waited on a process).
 *
 * @param PID id of the process
 */
bool wait(const UPID& pid);


/**
 * Invoke the thunk in a legacy safe way (i.e., outside of libprocess).
 *
 * @param thunk function to be invoked
 */
void invoke(const std::tr1::function<void(void)>& thunk);


/**
 * Use the specified filter on messages that get enqueued (note,
 * however, that for now you cannot filter timeout messages).
 *
 * @param filter message filter
 */
void filter(Filter* filter);


/**
 * Sends a message with data without a return address.
 *
 * @param to receiver
 * @param name message name
 * @param data data to send (gets copied)
 * @param length length of data
 */
void post(const UPID& to, const std::string& name, const char* data = NULL, size_t length = 0);


template <typename T>
void vdelegate(ProcessBase* process,
               std::tr1::function<void(T*)>* thunk)
{
  assert(process != NULL);
  assert(thunk != NULL);
  (*thunk)(static_cast<T*>(process));
  delete thunk;
}


template <typename R, typename T>
void delegate(ProcessBase* process,
              std::tr1::function<Promise<R>(T*)>* thunk,
              Future<R>* future)
{
  assert(process != NULL);
  assert(thunk != NULL);
  assert(future != NULL);
  (*thunk)(static_cast<T*>(process)).associate(*future);
  delete thunk;
  delete future;
}


/* Dispatches the delegator to the specified process. */
void dispatcher(const UPID& pid, std::tr1::function<void(ProcessBase*)>* delegator);


/**
 * Dispatches a void method on a process.
 *
 * @param pid receiver of message
 * @param method method to invoke on receiver
 */
template <typename T>
void dispatch(const PID<T>& pid, void (T::*method)())
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1));

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&vdelegate<T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk));

  dispatcher(pid, delegator);
}


/**
 * Dispatches a void method on a process.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 argument to pass to method
 */
template <typename T, typename P1, typename A1>
void dispatch(const PID<T>& pid, void (T::*method)(P1), A1 a1)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1));

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&vdelegate<T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk));

  dispatcher(pid, delegator);
}


/**
 * Dispatches a void method on a process.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 */
template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
void dispatch(const PID<T>& pid, void (T::*method)(P1, P2), A1 a1, A2 a2)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1, a2));

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&vdelegate<T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk));

  dispatcher(pid, delegator);
}


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
void dispatch(const PID<T>& pid, void (T::*method)(P1, P2, P3),
                       A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1, a2, a3));

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&vdelegate<T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk));

  dispatcher(pid, delegator);
}


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
void dispatch(const PID<T>& pid, void (T::*method)(P1, P2, P3, P4),
                       A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1, a2, a3, a4));

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&vdelegate<T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk));

  dispatcher(pid, delegator);
}


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
void dispatch(const PID<T>& pid, void (T::*method)(P1, P2, P3, P4, P5),
                       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<void(T*)>* thunk =
    new std::tr1::function<void(T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                     a1, a2, a3, a4, a5));

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&vdelegate<T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk));

  dispatcher(pid, delegator);
}


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T>
Future<R> dispatch(const PID<T>& pid, Promise<R> (T::*method)())
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&delegate<R, T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


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
Future<R> dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1), A1 a1)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&delegate<R, T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


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
Future<R> dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1, P2),
                            A1 a1, A2 a2)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1, a2));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&delegate<R, T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


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
Future<R> dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3),
                            A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1, a2, a3));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&delegate<R, T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


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
Future<R> dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3, P4),
                            A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1, a2, a3, a4));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&delegate<R, T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


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
Future<R> dispatch(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                            A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<Promise<R> (T*)>* thunk =
    new std::tr1::function<Promise<R> (T*)>(std::tr1::bind(method, std::tr1::placeholders::_1,
                                                           a1, a2, a3, a4, a5));

  Future<R>* future = new Future<R>();

  std::tr1::function<void(ProcessBase*)>* delegator =
    new std::tr1::function<void(ProcessBase*)>(std::tr1::bind(&delegate<R, T>,
                                                              std::tr1::placeholders::_1,
                                                              thunk, future));

  dispatcher(pid, delegator);

  return *future;
}


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @return result of executing the method
 */
template <typename R, typename T>
R call(const PID<T>& pid, Promise<R> (T::*method)())
{
  return dispatch(pid, method).get();
}


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
R call(const PID<T>& pid, Promise<R> (T::*method)(P1), A1 a1)
{
  return dispatch(pid, method, a1).get();
}


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
R call(const PID<T>& pid, Promise<R> (T::*method)(P1, P2), A1 a1, A2 a2)
{
  return dispatch(pid, method, a1, a2).get();
}


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
R call(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3),
                A1 a1, A2 a2, A3 a3)
{
  return dispatch(pid, method, a1, a2, a3).get();
}


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
R call(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3, P4),
                A1 a1, A2 a2, A3 a3, A4 a4)
{
  return dispatch(pid, method, a1, a2, a3, a4).get();
}


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
R call(const PID<T>& pid, Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return dispatch(pid, method, a1, a2, a3, a4, a5).get();
}

} // namespace process {

#endif // __PROCESS_HPP__
