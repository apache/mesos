/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#ifndef __PROCESS_PROCESS_HPP__
#define __PROCESS_PROCESS_HPP__

#include <stdint.h>

#include <map>
#include <queue>
#include <vector>

#include <process/address.hpp>
#include <process/clock.hpp>
#include <process/event.hpp>
#include <process/filter.hpp>
#include <process/firewall.hpp>
#include <process/http.hpp>
#include <process/message.hpp>
#include <process/mime.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/synchronized.hpp>
#include <stout/thread_local.hpp>

namespace process {

namespace firewall {

/**
 * Install a list of firewall rules which are used to forbid incoming
 * HTTP requests.
 *
 * The rules will be applied in the provided order to each incoming
 * request. If any rule forbids the request, no more rules are applied
 * and a "403 Forbidden" response will be returned containing the reason
 * from the rule.
 *
 * **NOTE**: if a request is forbidden, the request's handler is
 * not notified.
 *
 * @see process::firewall::FirewallRule
 *
 * @param rules List of rules which will be applied to all incoming
 *     HTTP requests.
 */
void install(std::vector<Owned<FirewallRule>>&& rules);

} // namespace firewall {

class ProcessBase : public EventVisitor
{
public:
  explicit ProcessBase(const std::string& id = "");

  virtual ~ProcessBase();

  UPID self() const { return pid; }

protected:
  /**
   * Invoked when an event is serviced.
   */
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

  /**
   * Invoked when a process gets spawned.
   */
  virtual void initialize() {}

  /**
   * Invoked when a process is terminated.
   *
   * **NOTE**: this does not get invoked automatically if
   * `process::ProcessBase::visit(const TerminateEvent&)` is overriden.
   */
  virtual void finalize() {}

  /**
   * Invoked when a linked process has exited.
   *
   * @see process::ProcessBase::link
   */
  virtual void exited(const UPID& pid) {}

  /**
   * Invoked when a linked process can no longer be monitored.
   *
   * @see process::ProcessBase::link
   */
  virtual void lost(const UPID& pid) {}

  /**
   * Puts the message at front of this process's message queue.
   *
   * @see process::Message
   */
  void inject(
      const UPID& from,
      const std::string& name,
      const char* data = NULL,
      size_t length = 0);

  /**
   * Sends the message to the specified `UPID`.
   *
   * @see process::Message
   */
  void send(
      const UPID& to,
      const std::string& name,
      const char* data = NULL,
      size_t length = 0);

  /**
   * Links with the specified `UPID`.
   *
   * Linking with a process from within the same "operating system
   * process" is guaranteed to give you perfect monitoring of that
   * process. However, linking with a process on another machine might
   * result in receiving lost callbacks due to the nature of a distributed
   * environment.
   */
  UPID link(const UPID& pid);

  /**
   * Any function which takes a "from" `UPID` and a message body as
   * arguments.
   *
   * The default visit implementation for message events invokes
   * installed message handlers, or delegates the message to another
   * process. A message handler always takes precedence over delegating.
   *
   * @see process::ProcessBase::install
   * @see process::ProcessBase::delegate
   */
  typedef lambda::function<void(const UPID&, const std::string&)>
  MessageHandler;

  /**
   * Sets up a handler for messages with the specified name.
   */
  void install(
      const std::string& name,
      const MessageHandler& handler)
  {
    handlers.message[name] = handler;
  }

  /**
   * @copydoc process::ProcessBase::install
   */
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

  /**
   * Delegates incoming messages, with the specified name, to the `UPID`.
   */
  void delegate(const std::string& name, const UPID& pid)
  {
    delegates[name] = pid;
  }

  /**
   * Any function which takes a `process::http::Request` and returns a
   * `process::http::Response`.
   *
   * The default visit implementation for HTTP events invokes
   * installed HTTP handlers.
   *
   * @see process::ProcessBase::route
   */
  typedef lambda::function<Future<http::Response>(const http::Request&)>
  HttpRequestHandler;

  /**
   * Sets up a handler for HTTP requests with the specified name.
   *
   * @param name The endpoint or URL to route.
   *     Must begin with a `/`.
   */
  void route(
      const std::string& name,
      const Option<std::string>& help,
      const HttpRequestHandler& handler);

  /**
   * @copydoc process::ProcessBase::route
   */
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

  /**
   * Sets up the default HTTP request handler to provide the static
   * asset(s) at the specified _absolute_ path for the specified name.
   *
   * For example, assuming the process named "server" invoked
   * `provide("name", "path")`, then an HTTP request for `/server/name`
   * would return the asset found at "path". If the specified path is a
   * directory then an HTTP request for `/server/name/file` would return
   * the asset found at `/path/file`.
   *
   * The `Content-Type` header of the HTTP response will be set to the
   * specified type given the file extension, which can be changed via
   * the optional `types` parameter.
   */
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

  /**
   * Returns the number of events of the given type currently on the event
   * queue.
   */
  template <typename T>
  size_t eventCount()
  {
    size_t count = 0U;

    synchronized (mutex) {
      count = std::count_if(events.begin(), events.end(), isEventType<T>);
    }

    return count;
  }

private:
  friend class SocketManager;
  friend class ProcessManager;
  friend class ProcessReference;
  friend void* schedule(void*);

  // Process states.
  enum
  {
    BOTTOM,
    READY,
    RUNNING,
    BLOCKED,
    TERMINATING,
    TERMINATED
  } state;

  template <typename T>
  static bool isEventType(const Event* event)
  {
    return event->is<T>();
  }

  // Mutex protecting internals.
  // TODO(benh): Consider replacing with a spinlock, on multi-core systems.
  std::recursive_mutex mutex;

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
  std::atomic_long refs;

  // Process PID.
  UPID pid;
};


template <typename T>
class Process : public virtual ProcessBase {
public:
  virtual ~Process() {}

  /**
   * Returns the `PID` of the process.
   *
   * Valid even before calling spawn.
   */
  PID<T> self() const { return PID<T>(dynamic_cast<const T*>(this)); }

protected:
  // Useful typedefs for dispatch/delay/defer to self()/this.
  typedef T Self;
  typedef T This;
};


/**
 * Initialize the library.
 *
 * **NOTE**: `libprocess` uses Google's `glog` and you can specify options
 * for it (e.g., a logging directory) via environment variables.
 *
 * @param delegate Process to receive root HTTP requests.
 *
 * @see [glog](https://google-glog.googlecode.com/svn/trunk/doc/glog.html)
 */
void initialize(const std::string& delegate = "");


/**
 * Clean up the library.
 */
void finalize();


/**
 * Get the request absolutePath path with delegate prefix.
 */
std::string absolutePath(const std::string& path);


/**
 * Returns the socket address associated with this instance of the library.
 */
network::Address address();


/**
 * Spawn a new process.
 *
 * @param process Process to be spawned.
 * @param manage Whether process should get garbage collected.
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
 * Sends a `TerminateEvent` to the given process.
 *
 * **NOTE**: currently, terminate only works for local processes (in the
 * future we plan to make this more explicit via the use of a `PID`
 * instead of a `UPID`).
 *
 * @param pid The process to terminate.
 * @param inject Whether the message should be injected ahead of all other
 *     messages queued up for that process.
 *
 * @see process::TerminateEvent
 */
void terminate(const UPID& pid, bool inject = true);
void terminate(const ProcessBase& process, bool inject = true);
void terminate(const ProcessBase* process, bool inject = true);


/**
 * Wait for the process to exit for no more than the specified seconds.
 *
 * @param PID ID of the process.
 * @param secs Max time to wait, 0 implies wait forever.
 *
 * @return true if a process was actually waited upon.
 */
bool wait(const UPID& pid, const Duration& duration = Seconds(-1));
bool wait(const ProcessBase& process, const Duration& duration = Seconds(-1));
bool wait(const ProcessBase* process, const Duration& duration = Seconds(-1));


/**
 * Sends a message with data without a return address.
 *
 * @param to Receiver of the message.
 * @param name Name of the message.
 * @param data Data to send (gets copied).
 * @param length Length of data.
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


/**
 * @copydoc process::terminate
 */
inline void terminate(const ProcessBase& process, bool inject)
{
  terminate(process.self(), inject);
}


/**
 * @copydoc process::terminate
 */
inline void terminate(const ProcessBase* process, bool inject)
{
  terminate(process->self(), inject);
}


/**
 * @copydoc process::wait
 */
inline bool wait(const ProcessBase& process, const Duration& duration)
{
  return process::wait(process.self(), duration); // Explicit to disambiguate.
}


/**
 * @copydoc process::wait
 */
inline bool wait(const ProcessBase* process, const Duration& duration)
{
  return process::wait(process->self(), duration); // Explicit to disambiguate.
}


// Per thread process pointer.
extern THREAD_LOCAL ProcessBase* __process__;

// NOTE: Methods in this namespace should only be used in tests to
// inject arbitrary events.
namespace inject {

/**
 * Simulates disconnection of the link between 'from' and 'to' by
 * sending an `ExitedEvent` to 'to'.
 *
 * @see process::ExitedEvent
 */
bool exited(const UPID& from, const UPID& to);

} // namespace inject {

} // namespace process {

#endif // __PROCESS_PROCESS_HPP__
