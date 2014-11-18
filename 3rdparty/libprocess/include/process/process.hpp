#ifndef __PROCESS_PROCESS_HPP__
#define __PROCESS_PROCESS_HPP__

#include <stdint.h>
#include <pthread.h>

#include <map>
#include <queue>

#include <process/clock.hpp>
#include <process/event.hpp>
#include <process/filter.hpp>
#include <process/http.hpp>
#include <process/message.hpp>
#include <process/mime.hpp>
#include <process/pid.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/thread.hpp>

namespace process {

class ProcessBase : public EventVisitor
{
public:
  explicit ProcessBase(const std::string& id = "");

  virtual ~ProcessBase();

  UPID self() const { return pid; }

protected:
  // Invoked when an event is serviced.
  virtual void serve(const Event& event)
  {
    event.visit(this);
  }

  // Callbacks used to visit (i.e., handle) a specific event.
  virtual void visit(const MessageEvent& event);
  virtual void visit(const DispatchEvent& event);
  virtual void visit(const HttpEvent& event);
  virtual void visit(const ExitedEvent& event);
  virtual void visit(const TerminateEvent& event);

  // Invoked when a process gets spawned.
  virtual void initialize() {}

  // Invoked when a process is terminated (unless visit is overriden).
  virtual void finalize() {}

  // Invoked when a linked process has exited (see link).
  virtual void exited(const UPID& pid) {}

  // Invoked when a linked process can no longer be monitored (see link).
  virtual void lost(const UPID& pid) {}

  // Puts a message at front of queue.
  void inject(
      const UPID& from,
      const std::string& name,
      const char* data = NULL,
      size_t length = 0);

  // Sends a message with data to PID.
  void send(
      const UPID& to,
      const std::string& name,
      const char* data = NULL,
      size_t length = 0);

  // Links with the specified PID. Linking with a process from within
  // the same "operating system process" is gauranteed to give you
  // perfect monitoring of that process. However, linking with a
  // process on another machine might result in receiving lost
  // callbacks due to the nature of a distributed environment.
  UPID link(const UPID& pid);

  // The default visit implementation for message events invokes
  // installed message handlers, or delegates the message to another
  // process (a delegate can be installed below but a message handler
  // always takes precedence over delegating). A message handler is
  // any function which takes two arguments, the "from" pid and the
  // message body.
  typedef lambda::function<void(const UPID&, const std::string&)>
  MessageHandler;

  // Setup a handler for a message.
  void install(
      const std::string& name,
      const MessageHandler& handler)
  {
    handlers.message[name] = handler;
  }

  template <typename T>
  void install(
      const std::string& name,
      void (T::*method)(const UPID&, const std::string&))
  {
    // Note that we use dynamic_cast here so a process can use
    // multiple inheritance if it sees so fit (e.g., to implement
    // multiple callback interfaces).
    MessageHandler handler =
      lambda::bind(method, dynamic_cast<T*>(this), lambda::_1, lambda::_2);
    install(name, handler);
  }

  // Delegate incoming message's with the specified name to pid.
  void delegate(const std::string& name, const UPID& pid)
  {
    delegates[name] = pid;
  }

  // The default visit implementation for HTTP events invokes
  // installed HTTP handlers. A HTTP handler is any function which
  // takes an http::Request object and returns an http::Response.
  typedef lambda::function<Future<http::Response>(const http::Request&)>
  HttpRequestHandler;

  // Setup a handler for an HTTP request.
  void route(
      const std::string& name,
      const Option<std::string>& help,
      const HttpRequestHandler& handler);

  template <typename T>
  void route(
      const std::string& name,
      const Option<std::string>& help,
      Future<http::Response> (T::*method)(const http::Request&))
  {
    // Note that we use dynamic_cast here so a process can use
    // multiple inheritance if it sees so fit (e.g., to implement
    // multiple callback interfaces).
    HttpRequestHandler handler =
      lambda::bind(method, dynamic_cast<T*>(this), lambda::_1);
    route(name, help, handler);
  }

  // Provide the static asset(s) at the specified _absolute_ path for
  // the specified name. For example, assuming the process named
  // "server" invoked 'provide("name", "path")' then an HTTP request
  // for '/server/name' would return the asset found at 'path'. If the
  // specified path is a directory then an HTTP request for
  // '/server/name/file' would return the asset found at
  // '/path/file'. The 'Content-Type' header of the HTTP response will
  // be set to the specified type given the file extension (you can
  // manipulate this via the optional 'types' parameter).
  void provide(
      const std::string& name,
      const std::string& path,
      const std::map<std::string, std::string>& types = mime::types)
  {
    // TODO(benh): Check that name is only alphanumeric (i.e., has no
    // '/') and that path is absolute.
    Asset asset;
    asset.path = path;
    asset.types = types;
    assets[name] = asset;
  }

  void lock()
  {
    pthread_mutex_lock(&m);
  }

  void unlock()
  {
    pthread_mutex_unlock(&m);
  }

  template<typename T>
  size_t eventCount()
  {
    size_t count = 0U;

    lock();
    count = std::count_if(events.begin(), events.end(), isEventType<T>);
    unlock();

    return count;
  }

private:
  friend class SocketManager;
  friend class ProcessManager;
  friend class ProcessReference;
  friend void* schedule(void*);

  // Process states.
  enum {
    BOTTOM,
    READY,
    RUNNING,
    BLOCKED,
    TERMINATING,
    TERMINATED
  } state;

  template<typename T>
  static bool isEventType(const Event* event)
  {
    return event->is<T>();
  }

  // Mutex protecting internals.
  // TODO(benh): Consider replacing with a spinlock, on multi-core systems.
  pthread_mutex_t m;

  // Enqueue the specified message, request, or function call.
  void enqueue(Event* event, bool inject = false);

  // Delegates for messages.
  std::map<std::string, UPID> delegates;

  // Handlers for messages and HTTP requests.
  struct {
    std::map<std::string, MessageHandler> message;
    std::map<std::string, HttpRequestHandler> http;
  } handlers;

  // Definition of a static asset.
  struct Asset
  {
    std::string path;
    std::map<std::string, std::string> types;
  };

  // Static assets(s) to provide.
  std::map<std::string, Asset> assets;

  // Queue of received events, requires lock()ed access!
  std::deque<Event*> events;

  // Active references.
  int refs;

  // Process PID.
  UPID pid;
};


template <typename T>
class Process : public virtual ProcessBase {
public:
  virtual ~Process() {}

  // Returns pid of process; valid even before calling spawn.
  PID<T> self() const { return PID<T>(dynamic_cast<const T*>(this)); }

protected:
  // Useful typedefs for dispatch/delay/defer to self()/this.
  typedef T Self;
  typedef T This;
};


/**
 * Initialize the library. Note that libprocess uses Google's glog and
 * you can specify options for it (e.g., a logging directory) via
 * environment variables (see the glog documentation for more
 * information).
 *
 * @param delegate process to receive root HTTP requests
 */
void initialize(const std::string& delegate = "");


/**
 * Clean up the library.
 */
void finalize();


/**
 * Returns the node associated with this instance of the library.
 */
Node node();


/**
 * Spawn a new process.
 *
 * @param process process to be spawned
 * @param manage boolean whether process should get garbage collected
 */
UPID spawn(ProcessBase* process, bool manage = false);

inline UPID spawn(ProcessBase& process, bool manage = false)
{
  return spawn(&process, manage);
}

template <typename T>
PID<T> spawn(T* t, bool manage = false)
{
  // We save the pid before spawn is called because it's possible that
  // the process has already been deleted after spawn returns (e.g.,
  // if 'manage' is true).
  PID<T> pid(t);

  if (!spawn(static_cast<ProcessBase*>(t), manage)) {
    return PID<T>();
  }

  return pid;
}

template <typename T>
PID<T> spawn(T& t, bool manage = false)
{
  return spawn(&t, manage);
}


/**
 * Send a TERMINATE message to a process, injecting the message ahead
 * of all other messages queued up for that process if requested. Note
 * that currently terminate only works for local processes (in the
 * future we plan to make this more explicit via the use of a PID
 * instead of a UPID).
 *
 * @param inject if true message will be put on front of message queue
 */
void terminate(const UPID& pid, bool inject = true);
void terminate(const ProcessBase& process, bool inject = true);
void terminate(const ProcessBase* process, bool inject = true);


/**
 * Wait for process to exit no more than specified seconds (returns
 * true if actually waited on a process).
 *
 * @param PID id of the process
 * @param secs max time to wait, 0 implies wait for ever
 */
bool wait(const UPID& pid, const Duration& duration = Seconds(-1));
bool wait(const ProcessBase& process, const Duration& duration = Seconds(-1));
bool wait(const ProcessBase* process, const Duration& duration = Seconds(-1));


/**
 * Sends a message with data without a return address.
 *
 * @param to receiver
 * @param name message name
 * @param data data to send (gets copied)
 * @param length length of data
 */
void post(const UPID& to,
          const std::string& name,
          const char* data = NULL,
          size_t length = 0);


void post(const UPID& from,
          const UPID& to,
          const std::string& name,
          const char* data = NULL,
          size_t length = 0);


// Inline implementations of above.
inline void terminate(const ProcessBase& process, bool inject)
{
  terminate(process.self(), inject);
}


inline void terminate(const ProcessBase* process, bool inject)
{
  terminate(process->self(), inject);
}


inline bool wait(const ProcessBase& process, const Duration& duration)
{
  return process::wait(process.self(), duration); // Explicit to disambiguate.
}


inline bool wait(const ProcessBase* process, const Duration& duration)
{
  return process::wait(process->self(), duration); // Explicit to disambiguate.
}


// Per thread process pointer. The extra level of indirection from
// _process_ to __process__ is used in order to take advantage of the
// ThreadLocal operators without needing the extra dereference.
extern ThreadLocal<ProcessBase>* _process_;

#define __process__ (*_process_)

// NOTE: Methods in this namespace should only be used in tests to
// inject arbitrary events.
namespace inject {
// Simulates disconnection of the link between 'from' and 'to' by
// sending an 'ExitedEvent' to 'to'.
bool exited(const UPID& from, const UPID& to);
} // namespace inject {

} // namespace process {

#endif // __PROCESS_PROCESS_HPP__
