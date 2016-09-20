// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

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

// Forward declaration.
class Logging;
class Sequence;

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
   * For local linked processes (i.e., when the linker and linkee are
   * part of the same OS process), this can be used to reliably detect
   * when the linked process has exited.
   *
   * For remote linked processes, this indicates that the persistent
   * TCP connection between the linker and the linkee has failed
   * (e.g., linkee process died, a network error occurred). In this
   * situation, the remote linkee process might still be running.
   *
   * @see process::ProcessBase::link
   */
  virtual void exited(const UPID&) {}

  /**
   * Invoked when a linked process can no longer be monitored.
   *
   * TODO(neilc): This is not implemented.
   *
   * @see process::ProcessBase::link
   */
  virtual void lost(const UPID&) {}

  /**
   * Puts the message at front of this process's message queue.
   *
   * @see process::Message
   */
  void inject(
      const UPID& from,
      const std::string& name,
      const char* data = nullptr,
      size_t length = 0);

  /**
   * Sends the message to the specified `UPID`.
   *
   * @see process::Message
   */
  void send(
      const UPID& to,
      const std::string& name,
      const char* data = nullptr,
      size_t length = 0);

  /**
   * Describes the behavior of the `link` call when the target `pid`
   * points to a remote process. This enum has no effect if the target
   * `pid` points to a local process.
   */
  enum class RemoteConnection
  {
    /**
     * If a persistent socket to the target `pid` does not exist,
     * a new link is created. If a persistent socket already exists,
     * `link` will subscribe this process to the existing link.
     *
     * This is the default behavior.
     */
    REUSE,

    /**
     * If a persistent socket to the target `pid` does not exist,
     * a new link is created. If a persistent socket already exists,
     * `link` create a new socket connection with the target `pid`
     * and *atomically* swap the existing link with the new link.
     *
     * Existing linkers will remain linked, albeit via the new socket.
     */
    RECONNECT,
  };

  /**
   * Links with the specified `UPID`.
   *
   * Linking with a process from within the same OS process is
   * guaranteed to give you perfect monitoring of that process.
   *
   * Linking to a remote process establishes a persistent TCP
   * connection to the remote libprocess instance that hosts that
   * process. If the TCP connection fails, the true state of the
   * remote linked process cannot be determined; we handle this
   * situation by generating an ExitedEvent.
   */
  UPID link(
      const UPID& pid,
      const RemoteConnection remote = RemoteConnection::REUSE);

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
   * Any function which takes a `process::http::Request` and an
   * `Option<std::string>` principal and returns a
   * `process::http::Response`.
   *
   * If the authentication principal string is set, the realm
   * requires authentication and authentication succeeded. If
   * it is not set, the realm does not require authentication.
   *
   * The default visit implementation for HTTP events invokes
   * installed HTTP handlers.
   *
   * @see process::ProcessBase::route
   */
  // TODO(arojas): Consider introducing an `authentication::Principal` type.
  typedef lambda::function<Future<http::Response>(
      const http::Request&, const Option<std::string>&)>
      AuthenticatedHttpRequestHandler;

  // TODO(arojas): Consider introducing an `authentication::Realm` type.
  void route(
      const std::string& name,
      const std::string& realm,
      const Option<std::string>& help,
      const AuthenticatedHttpRequestHandler& handler);

  /**
   * @copydoc process::ProcessBase::route
   */
  template <typename T>
  void route(
      const std::string& name,
      const std::string& realm,
      const Option<std::string>& help,
      Future<http::Response> (T::*method)(
          const http::Request&,
          const Option<std::string>&))
  {
    // Note that we use dynamic_cast here so a process can use
    // multiple inheritance if it sees so fit (e.g., to implement
    // multiple callback interfaces).
    AuthenticatedHttpRequestHandler handler =
      lambda::bind(method, dynamic_cast<T*>(this), lambda::_1, lambda::_2);
    route(name, realm, help, handler);
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

  // Definition of an HTTP endpoint. The endpoint can be
  // associated with an authentication realm, in which case:
  //
  //  (1) `realm` and `authenticatedHandler` will be set.
  //      Libprocess will perform HTTP authentication for
  //      all requests to this endpoint (by default during
  //      HttpEvent visitation). The authentication principal
  //      will be passed to the `authenticatedHandler`.
  //
  //  Otherwise, if the endpoint is not associated with an
  //  authentication realm:
  //
  //  (2) Only `handler` will be set, and no authentication
  //      takes place.
  struct HttpEndpoint
  {
    Option<HttpRequestHandler> handler;

    Option<std::string> realm;
    Option<AuthenticatedHttpRequestHandler> authenticatedHandler;
  };

  // Handlers for messages and HTTP requests.
  struct {
    std::map<std::string, MessageHandler> message;
    std::map<std::string, HttpEndpoint> http;

    // Used for delivering HTTP requests in the correct order.
    // Initialized lazily to avoid ProcessBase requiring
    // another Process!
    Owned<Sequence> httpSequence;
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
  PID<T> self() const { return PID<T>(static_cast<const T*>(this)); }

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
 * @param readwriteAuthenticationRealm The authentication realm that read-write
 *     libprocess-level HTTP endpoints will be installed under, if any.
 *     If this realm is not specified, read-write endpoints will be installed
 *     without authentication.
 * @param readonlyAuthenticationRealm The authentication realm that read-only
 *     libprocess-level HTTP endpoints will be installed under, if any.
 *     If this realm is not specified, read-only endpoints will be installed
 *     without authentication.
 * @return `true` if this was the first invocation of `process::initialize()`,
 *     or `false` if it was not the first invocation.
 *
 * @see [glog](https://google-glog.googlecode.com/svn/trunk/doc/glog.html)
 */
bool initialize(
    const Option<std::string>& delegate = None(),
    const Option<std::string>& readwriteAuthenticationRealm = None(),
    const Option<std::string>& readonlyAuthenticationRealm = None());


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
 * Return the PID associated with the global logging process.
 */
PID<Logging> logging();


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
          const char* data = nullptr,
          size_t length = 0);


void post(const UPID& from,
          const UPID& to,
          const std::string& name,
          const char* data = nullptr,
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
