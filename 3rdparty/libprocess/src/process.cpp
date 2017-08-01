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

#include <errno.h>
#include <limits.h>
#ifndef __WINDOWS__
#include <netdb.h>
#endif // __WINDOWS__
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __WINDOWS__
#include <unistd.h>

#include <arpa/inet.h>
#endif // __WINDOWS__

#include <glog/logging.h>

#ifndef __WINDOWS__
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#endif // __WINDOWS__

#include <algorithm>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory> // TODO(benh): Replace shared_ptr with unique_ptr.
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <process/address.hpp>
#include <process/check.hpp>
#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/executor.hpp>
#include <process/filter.hpp>
#include <process/future.hpp>
#include <process/gc.hpp>
#include <process/help.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/logging.hpp>
#include <process/mime.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/profiler.hpp>
#include <process/reap.hpp>
#include <process/sequence.hpp>
#include <process/socket.hpp>
#include <process/statistics.hpp>
#include <process/system.hpp>
#include <process/time.hpp>
#include <process/timer.hpp>

#include <process/metrics/metrics.hpp>

#include <process/ssl/flags.hpp>

#ifdef __WINDOWS__
#include <process/windows/jobobject.hpp>
#endif // __WINDOWS__

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/os/strerror.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/synchronized.hpp>

#include "authenticator_manager.hpp"
#include "config.hpp"
#include "decoder.hpp"
#include "encoder.hpp"
#include "event_loop.hpp"
#include "event_queue.hpp"
#include "gate.hpp"
#include "process_reference.hpp"
#include "run_queue.hpp"

namespace inet = process::network::inet;
namespace inet4 = process::network::inet4;
namespace inet6 = process::network::inet6;

using process::wait; // Necessary on some OS's to disambiguate.

using process::http::Accepted;
using process::http::BadRequest;
using process::http::Forbidden;
using process::http::InternalServerError;
using process::http::NotFound;
using process::http::OK;
using process::http::Request;
using process::http::Response;
using process::http::ServiceUnavailable;

using process::http::authentication::Authenticator;
using process::http::authentication::Principal;
using process::http::authentication::AuthenticationResult;
using process::http::authentication::AuthenticatorManager;

using process::http::authorization::AuthorizationCallbacks;

using process::network::inet::Address;
using process::network::inet::Socket;

using process::network::internal::SocketImpl;

using std::deque;
using std::find;
using std::list;
using std::map;
using std::ostream;
using std::pair;
using std::queue;
using std::set;
using std::stack;
using std::string;
using std::stringstream;
using std::vector;

namespace process {

namespace internal {

// These are environment variables expected in `process::initialize`.
// All these flags should be loaded with the prefix "LIBPROCESS_".
struct Flags : public virtual flags::FlagsBase
{
  Flags()
  {
    add(&Flags::ip,
        "ip",
        "The IP address for communication to and from libprocess.\n"
        "If not specified, libprocess will attempt to reverse-DNS lookup\n"
        "the hostname and use that IP instead.",
        [](const Option<net::IP>& ip) -> Option<Error> {
          if (ip.isSome() && ip->family() != AF_INET) {
            return Error(
              "Currently we allow only IPv4 address to be specified "
              "with the `--ip` flag");
          }

          return None();
        });

    add(&Flags::ip6,
        "ip6",
        "The IPv6 address that `libprocess` will use in future to perform "
        "communication of IPv6 sockets.\n");

    add(&Flags::advertise_ip,
        "advertise_ip",
        "The IP address that will be advertised to the outside world\n"
        "for communication to and from libprocess.  This is useful,\n"
        "for example, for containerized tasks in which communication\n"
        "is bound locally to a non-public IP that will be inaccessible\n"
        "to the master.");

    add(&Flags::port,
        "port",
        "The port for communication to and from libprocess.\n"
        "If not specified or set to 0, libprocess will bind it to a random\n"
        "available port.",
        [](const Option<int>& value) -> Option<Error> {
          if (value.isSome()) {
            if (value.get() < 0 || value.get() > USHRT_MAX) {
              return Error(
                  "LIBPROCESS_PORT=" + stringify(value.get()) +
                  " is not a valid port");
            }
          }

          return None();
        });

    add(&Flags::advertise_port,
        "advertise_port",
        "The port that will be advertised to the outside world\n"
        "for communication to and from libprocess.  NOTE: This port\n"
        "will not actually be bound (only the local '--port' will be), so\n"
        "redirection to the local IP and port must be provided separately.",
        [](const Option<int>& value) -> Option<Error> {
          if (value.isSome()) {
            if (value.get() <= 0 || value.get() > USHRT_MAX) {
              return Error(
                  "LIBPROCESS_ADVERTISE_PORT=" + stringify(value.get()) +
                  " is not a valid port");
            }
          }

          return None();
        });

    add(&Flags::require_peer_address_ip_match,
        "require_peer_address_ip_match",
        "If set, the IP address portion of the libprocess UPID in\n"
        "incoming messages is required to match the IP address of\n"
        "the socket from which the message was sent. This can be a\n"
        "security enhancement since it prevents unauthorized senders\n"
        "impersonating other libprocess actors. This check may\n"
        "break configurations that require setting LIBPROCESS_IP,\n"
        "or LIBPROCESS_ADVERTISE_IP. Additionally, multi-homed\n"
        "configurations may be affected since the address on which\n"
        "libprocess is listening may not match the address from\n"
        "which libprocess connects to other actors.\n",
        false);
  }

  Option<net::IP> ip;
  Option<net::IPv6> ip6;
  Option<net::IP> advertise_ip;
  Option<int> port;
  Option<int> advertise_port;
  bool require_peer_address_ip_match;
};

} // namespace internal {

namespace ID {

string generate(const string& prefix)
{
  static map<string, int>* prefixes = new map<string, int>();
  static std::mutex* prefixes_mutex = new std::mutex();

  int id;
  synchronized (prefixes_mutex) {
    int& _id = (*prefixes)[prefix];
    _id += 1;
    id = _id;
  }
  return prefix + "(" + stringify(id) + ")";
}

} // namespace ID {


namespace mime {

map<string, string> types;

} // namespace mime {


// Provides a process that manages sending HTTP responses so as to
// satisfy HTTP/1.1 pipelining. Each request should either enqueue a
// response, or ask the proxy to handle a future response. The process
// is responsible for making sure the responses are sent in the same
// order as the requests. Note that we use a 'Socket' in order to keep
// the underlying file descriptor from getting closed while there
// might still be outstanding responses even though the client might
// have closed the connection (see more discussion in
// SocketManager::close and SocketManager::proxy).
class HttpProxy : public Process<HttpProxy>
{
public:
  explicit HttpProxy(const Socket& _socket);
  virtual ~HttpProxy() {}

  // Enqueues the response to be sent once all previously enqueued
  // responses have been processed (e.g., waited for and sent).
  void enqueue(const Response& response, const Request& request);

  // Enqueues a future to a response that will get waited on (up to
  // some timeout) and then sent once all previously enqueued
  // responses have been processed (e.g., waited for and sent).
  void handle(const Future<Response>& future, const Request& request);

protected:
  void finalize() override;

private:
  // Starts "waiting" on the next available future response.
  void next();

  // Invoked once a future response has been satisfied.
  void waited(const Future<Response>& future);

  // Demuxes and handles a response.
  bool process(const Future<Response>& future, const Request& request);

  // Handles stream based responses.
  void stream(const Owned<Request>& request, const Future<string>& chunk);

  Socket socket; // Wrap the socket to keep it from getting closed.

  // Describes a queue "item" that wraps the future to the response
  // and the original request.
  // The original request contains needed information such as what encodings
  // are acceptable and whether to persist the connection.
  struct Item
  {
    Item(const Request& _request, const Future<Response>& _future)
      : request(_request), future(_future) {}

    const Request request; // Make a copy.
    Future<Response> future; // Make a copy.
  };

  queue<Item*> items;

  Option<http::Pipe::Reader> pipe; // Current pipe, if streaming.
};


// Helper for creating routes without a process.
// TODO(benh): Move this into route.hpp.
class Route
{
public:
  Route(const string& name,
        const Option<string>& help,
        const lambda::function<Future<Response>(const Request&)>& handler)
    : process(name, help, handler)
  {
    spawn(process);
  }

  ~Route()
  {
    terminate(process);
    wait(process);
  }

private:
  class RouteProcess : public Process<RouteProcess>
  {
  public:
    RouteProcess(
        const string& name,
        const Option<string>& _help,
        const lambda::function<Future<Response>(const Request&)>& _handler)
      : ProcessBase(strings::remove(name, "/", strings::PREFIX)),
        help(_help),
        handler(_handler) {}

  protected:
    virtual void initialize()
    {
      route("/", help, &RouteProcess::handle);
    }

    Future<Response> handle(const Request& request)
    {
      return handler(request);
    }

    const Option<string> help;
    const lambda::function<Future<Response>(const Request&)> handler;
  };

  RouteProcess process;
};


class SocketManager
{
public:
  SocketManager();
  ~SocketManager();

  // Closes all managed sockets and clears any associated metadata.
  // The `__s__` server socket must be closed and `ProcessManager`
  // must be finalized before calling this.
  void finalize();

  void accepted(const Socket& socket);

  void link(ProcessBase* process,
            const UPID& to,
            const ProcessBase::RemoteConnection remote,
            const SocketImpl::Kind& kind = SocketImpl::DEFAULT_KIND());

  // Test-only method to fetch the file descriptor behind a
  // persistent socket.
  Option<int_fd> get_persistent_socket(const UPID& to);

  PID<HttpProxy> proxy(const Socket& socket);

  // Used to clean up the pointer to an `HttpProxy` in case the
  // `HttpProxy` is killed outside the control of the `SocketManager`.
  // This generally happens when `process::finalize` is called.
  void unproxy(const Socket& socket);

  void send(Encoder* encoder, bool persist, const Socket& socket);
  void send(const Response& response,
            const Request& request,
            const Socket& socket);
  void send(Message&& message,
            const SocketImpl::Kind& kind = SocketImpl::DEFAULT_KIND());

  Encoder* next(int_fd s);

  void close(int_fd s);

  void exited(const Address& address);
  void exited(ProcessBase* process);

private:
  // TODO(bmahler): Leverage a bidirectional multimap instead, or
  // hide the complexity of manipulating 'links' through methods.
  struct
  {
    // For links, we maintain a bidirectional mapping between the
    // "linkers" (Processes) and the "linkees" (remote / local UPIDs).
    // For remote socket addresses, we also need a mapping to the
    // linkees for that socket address, because socket closure only
    // notifies at the address level.
    hashmap<UPID, hashset<ProcessBase*>> linkers;
    hashmap<ProcessBase*, hashset<UPID>> linkees;
    hashmap<Address, hashset<UPID>> remotes;
  } links;

  // Switch the underlying socket that a remote end is talking to.
  // This manipulates the data structures below by swapping all data
  // mapped to 'from' to being mapped to 'to'. This is useful for
  // downgrading a socket from SSL to POLL based.
  void swap_implementing_socket(const Socket& from, const Socket& to);

  // Helper function for link().
  void link_connect(
      const Future<Nothing>& future,
      Socket socket,
      const UPID& to);

  // Helper function for send().
  void send_connect(
      const Future<Nothing>& future,
      Socket socket,
      Message&& message);

  // Collection of all active sockets (both inbound and outbound).
  hashmap<int_fd, Socket> sockets;

  // Collection of sockets that should be disposed when they are
  // finished being used (e.g., when there is no more data to send on
  // them). Can contain both inbound and outbound sockets.
  hashset<int_fd> dispose;

  // Map from socket to socket address for outbound sockets.
  hashmap<int_fd, Address> addresses;

  // Map from socket address to temporary sockets (outbound sockets
  // that will be closed once there is no more data to send on them).
  hashmap<Address, int_fd> temps;

  // Map from socket address (ip, port) to persistent sockets
  // (outbound sockets that will remain open even if there is no more
  // data to send on them).  We distinguish these from the 'temps'
  // collection so we can tell when a persistent socket has been lost
  // (and thus generate ExitedEvents).
  hashmap<Address, int_fd> persists;

  // Map from outbound socket to outgoing queue.
  hashmap<int_fd, queue<Encoder*>> outgoing;

  // HTTP proxies.
  hashmap<int_fd, HttpProxy*> proxies;

  // Protects instance variables.
  std::recursive_mutex mutex;
};


class ProcessManager
{
public:
  explicit ProcessManager(const Option<string>& delegate);
  ~ProcessManager();

  // Prevents any further processes from spawning and terminates all
  // running processes. The special `gc` process will be terminated
  // last. Then joins all processing threads and stops the event loop.
  //
  // This is a prerequisite for finalizing the `SocketManager`.
  void finalize();

  // Initializes the processing threads and the event loop thread,
  // and returns the number of processing threads created.
  long init_threads();

  ProcessReference use(const UPID& pid);

  void handle(
      const Socket& socket,
      Request* request);

  bool deliver(
      ProcessBase* receiver,
      Event* event,
      ProcessBase* sender = nullptr);

  bool deliver(
      const UPID& to,
      Event* event,
      ProcessBase* sender = nullptr);

  // TODO(josephw): Change the return type to a `Try<UPID>`. Currently,
  // if this method fails, we return a default constructed `UPID`.
  UPID spawn(ProcessBase* process, bool manage);

  void resume(ProcessBase* process);
  void cleanup(ProcessBase* process);

  void link(
      ProcessBase* process,
      const UPID& to,
      const ProcessBase::RemoteConnection remote);

  void terminate(const UPID& pid, bool inject, ProcessBase* sender = nullptr);
  bool wait(const UPID& pid);

  void installFirewall(vector<Owned<firewall::FirewallRule>>&& rules);
  string absolutePath(const string& path);

  void enqueue(ProcessBase* process);
  ProcessBase* dequeue();

  void settle();

  // The /__processes__ route.
  Future<Response> __processes__(const Request&);

  void install(Filter* f)
  {
    // NOTE: even though `filter` is atomic we still need to
    // synchronize updating it because once we return from this
    // function the old filter might get deleted which could be bad if
    // a thread is currently using the old filter in
    // `ProcessManager::resume`.
    synchronized (filter_mutex) {
      filter.store(f);
    }
  }

  long workers() const
  {
    return threads.size() - 1; // Less 1 for event loop thread.
  }

private:
  // Delegate process name to receive root HTTP requests.
  const Option<string> delegate;

  // Map of all local spawned and running processes.
  hashmap<string, ProcessBase*> processes;
  std::recursive_mutex processes_mutex;

  // Queue of runnable processes.
  //
  // See run_queue.hpp for more information about the run queue
  // implementation.
  RunQueue runq;

  // Number of running processes, to support Clock::settle operation.
  std::atomic_long running;

  // Stores the thread handles so that we can join during shutdown.
  vector<std::thread*> threads;

  // Boolean used to signal processing threads to stop running.
  std::atomic_bool joining_threads;

  // List of rules applied to all incoming HTTP requests.
  vector<Owned<firewall::FirewallRule>> firewallRules;
  std::recursive_mutex firewall_mutex;

  // Whether the process manager is finalizing or not.
  // If true, no further processes will be spawned.
  std::atomic_bool finalizing;

  // Filter. Synchronized support for using the filter needs to be
  // recursive in case a filter wants to do anything fancy (which is
  // possible and likely given that filters will get used for
  // testing).
  std::atomic<Filter*> filter = ATOMIC_VAR_INIT(nullptr);
  std::recursive_mutex filter_mutex;
};


static internal::Flags* libprocess_flags = new internal::Flags();

// Synchronization primitives for `initialize`.
// See documentation in `initialize` for how they are used.
static std::atomic_bool initialize_started(false);
static std::atomic_bool initialize_complete(false);

// Server socket listen backlog.
static const int LISTEN_BACKLOG = 500000;

// Local server socket.
static Socket* __s__ = nullptr;

// This mutex is only used to prevent a race between the `on_accept`
// callback loop and closing/deleting `__s__` in `process::finalize`.
static std::mutex* socket_mutex = new std::mutex();

// The future returned by the last call to `__s__->accept()`.
// This is used in `process::finalize` to explicitly terminate the
// `__s__` socket's callback loop.
static Future<Socket> future_accept;

// Local socket address.
static inet::Address __address__ = inet4::Address::ANY_ANY();

// Local IPv6 socket address.
static Option<inet6::Address> __address6__ = None();

// Active SocketManager (eventually will probably be thread-local).
static SocketManager* socket_manager = nullptr;

// Active ProcessManager (eventually will probably be thread-local).
static ProcessManager* process_manager = nullptr;

// Used for authenticating HTTP requests.
static AuthenticatorManager* authenticator_manager = nullptr;

// Authorization callbacks for HTTP endpoints.
static AuthorizationCallbacks* authorization_callbacks = nullptr;

// Global route that returns process information.
static Route* processes_route = nullptr;

// Global garbage collector.
GarbageCollector* gc = nullptr;

// Global help.
PID<Help> help;

// Global logging.
PID<Logging> _logging;

// Per-thread process pointer.
thread_local ProcessBase* __process__ = nullptr;

// Per-thread executor pointer.
thread_local Executor* _executor_ = nullptr;

namespace metrics {
namespace internal {

PID<metrics::internal::MetricsProcess> metrics;

} // namespace internal {
} // namespace metrics {

namespace internal {

// Global reaper.
PID<process::internal::ReaperProcess> reaper;

// Global job object manager.
#ifdef __WINDOWS__
PID<process::internal::JobObjectManager> job_object_manager;
#endif // __WINDOWS__

} // namespace internal {


namespace http {

namespace authentication {

Future<Nothing> setAuthenticator(
    const string& realm,
    Owned<Authenticator> authenticator)
{
  process::initialize();

  return authenticator_manager->setAuthenticator(realm, authenticator);
}


Future<Nothing> unsetAuthenticator(const string& realm)
{
  process::initialize();

  return authenticator_manager->unsetAuthenticator(realm);
}

} // namespace authentication {

namespace authorization {

void setCallbacks(const AuthorizationCallbacks& callbacks)
{
  if (authorization_callbacks != nullptr) {
    delete authorization_callbacks;
  }

  authorization_callbacks = new AuthorizationCallbacks(callbacks);
}


void unsetCallbacks()
{
  if (authorization_callbacks != nullptr) {
    delete authorization_callbacks;
  }

  authorization_callbacks = nullptr;
}

} // namespace authorization {

} // namespace http {


// NOTE: Clock::* implementations are in clock.cpp except for
// Clock::settle which currently has a dependency on
// 'process_manager'.
void Clock::settle()
{
  process_manager->settle();
}


static Message encode(
    const UPID& from,
    const UPID& to,
    const string& name,
    const char* data,
    size_t length)
{
  Message message{name, from, to, string(data, length)};
  return message;
}


static Message encode(
    const UPID& from,
    const UPID& to,
    string&& name,
    const char* data,
    size_t length)
{
  Message message{std::move(name), from, to, string(data, length)};
  return message;
}


static void transport(Message&& message, ProcessBase* sender = nullptr)
{
  if (message.to.address == __address__) {
    // Local message.
    MessageEvent* event = new MessageEvent(std::move(message));
    process_manager->deliver(event->message.to, event, sender);
  } else {
    // Remote message.
    socket_manager->send(std::move(message));
  }
}


static void transport(
    const UPID& from,
    const UPID& to,
    const string& name,
    const char* data,
    size_t length,
    ProcessBase* sender = nullptr)
{
  if (to.address == __address__) {
    // Local message.
    MessageEvent* event = new MessageEvent(
        from,
        to,
        name,
        data,
        length);

    process_manager->deliver(event->message.to, event, sender);
  } else {
    // Remote message.
    socket_manager->send(encode(from, to, name, data, length));
  }
}


static void transport(
    const UPID& from,
    const UPID& to,
    string&& name,
    const char* data,
    size_t length,
    ProcessBase* sender = nullptr)
{
  if (to.address == __address__) {
    // Local message.
    MessageEvent* event = new MessageEvent(
        from,
        to,
        std::move(name),
        data,
        length);

    process_manager->deliver(event->message.to, event, sender);
  } else {
    // Remote message.
    socket_manager->send(encode(from, to, std::move(name), data, length));
  }
}


// Returns true if `request` contains an inbound libprocess message.
// A libprocess message can either be sent by another instance of
// libprocess (i.e. both of the "User-Agent" and "Libprocess-From"
// headers will be set), or a client that speaks the libprocess
// protocol (i.e. only the "Libprocess-From" header will be set).
// This function returns true for either case.
static bool libprocess(Request* request)
{
  return
    (request->method == "POST" &&
     request->headers.contains("User-Agent") &&
     request->headers["User-Agent"].find("libprocess/") == 0) ||
    (request->method == "POST" &&
     request->headers.contains("Libprocess-From"));
}


// Returns a 'BODY' request once the body of the provided
// 'PIPE' request can be read completely.
static Future<Owned<Request>> convert(Owned<Request>&& pipeRequest)
{
  CHECK_EQ(Request::PIPE, pipeRequest->type);
  CHECK_SOME(pipeRequest->reader);
  CHECK(pipeRequest->body.empty());

  return pipeRequest->reader->readAll()
    .then([pipeRequest](const string& body) -> Future<Owned<Request>> {
      pipeRequest->type = Request::BODY;
      pipeRequest->body = body;
      pipeRequest->reader = None(); // Remove the reader.

      return pipeRequest;
    });
}


static Future<MessageEvent*> parse(const Request& request)
{
  // TODO(benh): Do better error handling (to deal with a malformed
  // libprocess message, malicious or otherwise).

  // First try and determine 'from'.
  Option<UPID> from = None();

  if (request.headers.contains("Libprocess-From")) {
    from = UPID(strings::trim(request.headers.at("Libprocess-From")));
  } else {
    // Try and get 'from' from the User-Agent.
    const string& agent = request.headers.at("User-Agent");
    const string identifier = "libprocess/";
    size_t index = agent.find(identifier);
    if (index != string::npos) {
      from = UPID(agent.substr(index + identifier.size(), agent.size()));
    }
  }

  if (from.isNone()) {
    return Failure("Failed to determine sender from request headers");
  }

  // Check that URL path is present and starts with '/'.
  if (request.url.path.find('/') != 0) {
    return Failure("Request URL path must start with '/'");
  }

  // Now determine 'to'.
  size_t index = request.url.path.find('/', 1);
  index = index != string::npos ? index - 1 : string::npos;

  // Decode possible percent-encoded 'to'.
  Try<string> decode = http::decode(request.url.path.substr(1, index));

  if (decode.isError()) {
    return Failure("Failed to decode URL path: " + decode.error());
  }

  const UPID to(decode.get(), __address__);

  // And now determine 'name'.
  index = index != string::npos ? index + 2: request.url.path.size();
  const string name = request.url.path.substr(index);

  VLOG(2) << "Parsed message name '" << name
          << "' for " << to << " from " << from.get();

  CHECK_SOME(request.reader);
  http::Pipe::Reader reader = request.reader.get(); // Remove const.

  return reader.readAll()
    .then([from, name, to](const string& body) {
      Message message;
      message.name = name;
      message.from = from.get();
      message.to = to;
      message.body = body;

      return new MessageEvent(std::move(message));
    });
}


namespace internal {

void decode_recv(
    const Future<size_t>& length,
    char* data,
    size_t size,
    Socket socket,
    StreamingRequestDecoder* decoder)
{
  if (length.isDiscarded() || length.isFailed()) {
    if (length.isFailed()) {
      VLOG(1) << "Decode failure: " << length.failure();
    }

    socket_manager->close(socket);
    delete[] data;
    delete decoder;
    return;
  }

  if (length.get() == 0) {
    socket_manager->close(socket);
    delete[] data;
    delete decoder;
    return;
  }

  // Decode as much of the data as possible into HTTP requests.
  const deque<Request*> requests = decoder->decode(data, length.get());

  if (requests.empty() && decoder->failed()) {
     VLOG(1) << "Decoder error while receiving";
     socket_manager->close(socket);
     delete[] data;
     delete decoder;
     return;
  }

  if (!requests.empty()) {
    // Get the peer address to augment the requests.
    Try<Address> address = socket.peer();

    if (address.isError()) {
      VLOG(1) << "Failed to get peer address while receiving: "
              << address.error();
      socket_manager->close(socket);
      delete[] data;
      delete decoder;
      return;
    }

    foreach (Request* request, requests) {
      request->client = address.get();
      process_manager->handle(socket, request);
    }
  }

  socket.recv(data, size)
    .onAny(lambda::bind(&decode_recv, lambda::_1, data, size, socket, decoder));
}

} // namespace internal {


void timedout(const list<Timer>& timers)
{
  // Update current time of process (if it's present/valid). Note that
  // current time may be greater than the timeout if a local message
  // was received (and happens-before kicks in).
  if (Clock::paused()) {
    foreach (const Timer& timer, timers) {
      if (ProcessReference process = process_manager->use(timer.creator())) {
        Clock::update(process, timer.timeout().time());
      }
    }
  }

  // Invoke the timers that timed out (TODO(benh): Do this
  // asynchronously so that we don't tie up the event thread!).
  foreach (const Timer& timer, timers) {
    timer();
  }
}


// We might find value in catching terminating signals at some point.
// However, for now, adding signal handlers freely is not allowed
// because they will clash with Java and Python virtual machines and
// causes hard to debug crashes/segfaults.

// void sigbad(int signal, struct sigcontext *ctx)
// {
//   // Pass on the signal (so that a core file is produced).
//   struct sigaction sa;
//   sa.sa_handler = SIG_DFL;
//   sigemptyset(&sa.sa_mask);
//   sa.sa_flags = 0;
//   sigaction(signal, &sa, nullptr);
//   raise(signal);
// }


namespace internal {

void on_accept(const Future<Socket>& socket)
{
  if (socket.isReady()) {
    // Inform the socket manager for proper bookkeeping.
    socket_manager->accepted(socket.get());

    const size_t size = 80 * 1024;
    char* data = new char[size];

    StreamingRequestDecoder* decoder = new StreamingRequestDecoder();

    socket.get().recv(data, size)
      .onAny(lambda::bind(
          &internal::decode_recv,
          lambda::_1,
          data,
          size,
          socket.get(),
          decoder));
  } else {
     LOG(INFO) << "Failed to accept socket: "
               << (socket.isFailed() ? socket.failure() : "future discarded");
  }

  // NOTE: `__s__` may be cleaned up during `process::finalize`.
  synchronized (socket_mutex) {
    if (__s__ != nullptr) {
      future_accept = __s__->accept()
        .onAny(lambda::bind(&on_accept, lambda::_1));
    }
  }
}

} // namespace internal {


namespace firewall {

void install(vector<Owned<FirewallRule>>&& rules)
{
  process::initialize();

  process_manager->installFirewall(std::move(rules));
}

} // namespace firewall {


// Tests can declare this function and use it to re-configure libprocess
// programmatically. Without explicitly declaring this function, it
// is not visible. This is the preferred behavior as we do not want
// applications changing these settings while they are
// running (this would be undefined behavior).
// NOTE: If the test is changing SSL-related configuration, the SSL library
// must be reinitialized first.  See `reinitialize` in `openssl.cpp`.
void reinitialize(
    const Option<string>& delegate,
    const Option<string>& readwriteAuthenticationRealm,
    const Option<string>& readonlyAuthenticationRealm)
{
  process::finalize();

  // Reset the initialization synchronization primitives.
  initialize_started.store(false);
  initialize_complete.store(false);

  process::initialize(
      delegate,
      readwriteAuthenticationRealm,
      readonlyAuthenticationRealm);
}


bool initialize(
    const Option<string>& delegate,
    const Option<string>& readwriteAuthenticationRealm,
    const Option<string>& readonlyAuthenticationRealm)
{
  // TODO(benh): Return an error if attempting to initialize again
  // with a different delegate than originally specified.

  // NOTE: Rather than calling `initialize` once at the root of the
  // dependency tree; we currently rely on libprocess dependency
  // declaration by invoking `initialize` prior to use. This is done
  // frequently throughout the code base. Therefore we chose to use
  // atomics rather than `Once`, as the overhead of a mutex and
  // condition variable is excessive here.

  // Try and do the initialization or wait for it to complete.

  // If already initialized, there's nothing more to do.
  // NOTE: This condition is true as soon as the thread performing
  // initialization sets `initialize_complete` to `true` in the *middle*
  // of initialization.  This is done because some methods called by
  // initialization will themselves call `process::initialize`.
  if (initialize_started.load() && initialize_complete.load()) {
    // Return `false` because `process::initialize()` was already called.
    return false;

  } else {
    // NOTE: `compare_exchange_strong` needs an lvalue.
    bool expected = false;

    // Any thread that calls `initialize` prior to when `initialize_complete`
    // is set to `true` will reach this.

    // Atomically sets `initialize_started` to `true`.  The thread that
    // successfully sets `initialize_started` to `true` will move on to
    // perform the initialization logic.  All others will wait here for
    // initialization to complete.
    if (!initialize_started.compare_exchange_strong(expected, true)) {
      while (!initialize_complete.load());

      // Return `false` because `process::initialize()` was already called.
      return false;
    }
  }

#ifdef __WINDOWS__
  // Initialize the Windows socket stack. This operation is idempotent.
  // NOTE: This call can report an error here if it determines it is
  // incompatible with the WSA version, so it is important to call this
  // even if we expect users of libprocess to have already started the
  // socket stack themselves. We exit the process under error condition
  // to prevent cryptic errors later.
  if (!net::wsa_initialize()) {
    EXIT(EXIT_FAILURE) << "WSA failed to initialize";
  }
#endif // __WINDOWS__

  // We originally tried to leave SIGPIPE unblocked and to work
  // around SIGPIPE in order to avoid imposing policy on users
  // of libprocess. However, for pipes and files, the manual
  // suppression of SIGPIPE had become onerous. Also, OS X
  // appears to deliver SIGPIPE to the process rather than
  // the triggering thread. It is better to just silence it
  // and use EPIPE instead. See MESOS-2079 and related tickets.
  //
  // TODO(bmahler): Should libprocess finalization restore the
  // previous handler?
  //
  // TODO(bmahler): Consider removing SO_NOSIGPIPE and MSG_NOSIGNAL
  // to avoid confusion, now that they are no longer relevant.
  signal(SIGPIPE, SIG_IGN);

#ifdef USE_SSL_SOCKET
  // Notify users of the 'LIBPROCESS_SSL_SUPPORT_DOWNGRADE' flag that
  // this setting allows insecure connections.
  if (network::openssl::flags().support_downgrade) {
    LOG(WARNING) <<
      "Failed SSL connections will be downgraded to a non-SSL socket";
  }
#endif

  // Create a new ProcessManager and SocketManager.
  process_manager = new ProcessManager(delegate);
  socket_manager = new SocketManager();

  // Initialize the event loop.
  EventLoop::initialize();

  // Setup processing threads.
  long num_worker_threads = process_manager->init_threads();

  Clock::initialize(lambda::bind(&timedout, lambda::_1));

  // Fill in the local IP and port for inter-libprocess communication.
  __address__ = inet4::Address::ANY_ANY();

  // Fetch and parse the libprocess environment variables.
  Try<flags::Warnings> load = libprocess_flags->load("LIBPROCESS_");

  if (load.isError()) {
    EXIT(EXIT_FAILURE) << libprocess_flags->usage(load.error());
  }

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  uint16_t port = 0;

  if (libprocess_flags->port.isSome()) {
    port = libprocess_flags->port.get();
    __address__.port = port;
  }

  if (libprocess_flags->ip.isSome()) {
    __address__.ip = libprocess_flags->ip.get();
  }

  if (libprocess_flags->ip6.isSome()) {
    __address6__ = inet6::Address(libprocess_flags->ip6.get(), port);
  }

  // Create a "server" socket for communicating.
  Try<Socket> create = Socket::create();
  if (create.isError()) {
    LOG(FATAL) << "Failed to construct server socket:" << create.error();
  }
  __s__ = new Socket(create.get());

  // Allow address reuse.
  // NOTE: We cast to `char*` here because the function prototypes on Windows
  // use `char*` instead of `void*`.
  int on = 1;
  if (::setsockopt(
          __s__->get(),
          SOL_SOCKET,
          SO_REUSEADDR,
          reinterpret_cast<char*>(&on),
          sizeof(on)) < 0) {
    PLOG(FATAL) << "Failed to initialize, setsockopt(SO_REUSEADDR)";
  }

  Try<Address> bind = __s__->bind(__address__);
  if (bind.isError()) {
    LOG(FATAL) << "Failed to initialize: " << bind.error();
  }

  __address__ = bind.get();

  // If advertised IP and port are present, use them instead.
  if (libprocess_flags->advertise_ip.isSome()) {
    __address__.ip = libprocess_flags->advertise_ip.get();
  }

  if (libprocess_flags->advertise_port.isSome()) {
    __address__.port = libprocess_flags->advertise_port.get();
  }

  // Resolve the hostname if ip is 0.0.0.0 in case we actually have
  // a valid external IP address. Note that we need only one IP
  // address, so that other processes can send and receive and
  // don't get confused as to whom they are sending to.
  if (__address__.ip.isAny()) {
    char hostname[512];

    if (gethostname(hostname, sizeof(hostname)) < 0) {
      PLOG(FATAL) << "Failed to initialize, gethostname";
    }

    // Lookup an IP address of local hostname, taking the first result.
    Try<net::IP> ip = net::getIP(hostname, __address__.ip.family());

    if (ip.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to obtain the IP address for '" << hostname << "';"
        << " the DNS service may not be able to resolve it: " << ip.error();
    }

    __address__.ip = ip.get();
  }

  Try<Nothing> listen = __s__->listen(LISTEN_BACKLOG);
  if (listen.isError()) {
    LOG(FATAL) << "Failed to initialize: " << listen.error();
  }

  // Need to set `initialize_complete` here so that we can actually
  // invoke `accept()` and `spawn()` below.
  initialize_complete.store(true);

  future_accept = __s__->accept()
    .onAny(lambda::bind(&internal::on_accept, lambda::_1));

  // TODO(benh): Make sure creating the garbage collector, logging
  // process, and profiler always succeeds and use supervisors to make
  // sure that none terminate.

  // For the global processes below, the order of initialization matters.
  // Some global processes are necessary for the function of certain methods:
  //
  //   process | Underpins this method
  //   --------|---------------------------
  //   gc      | process::spawn(..., true)
  //   help    | ProcessBase::route(...)
  //   metrics | process::metrics::add(...)
  //
  // Due to the above, each global process must be started after the
  // prerequisite global process(es) have been started. The following
  // graph shows what processes depend on which other processes.
  // Processes in the same vertical group can be safely started in any order.
  //
  //   gc
  //   |--help
  //   |  |--metrics
  //   |  |  |--system
  //   |  |  |--job_object_manager (Windows only)
  //   |  |  |--All other processes
  //   |  |
  //   |  |--logging
  //   |  |--profiler
  //   |  |--processesRoute
  //   |
  //   |--authentication_manager

  // Create global garbage collector process.
  gc = new GarbageCollector();
  spawn(gc);

  // Create global help process.
  help = spawn(new Help(delegate), true);

  // Create the global metrics process.
  metrics::internal::metrics = spawn(
      metrics::internal::MetricsProcess::create(readonlyAuthenticationRealm),
      true);

  // Create the global logging process.
  _logging = spawn(new Logging(readwriteAuthenticationRealm), true);

  // Create the global profiler process.
  spawn(new Profiler(readwriteAuthenticationRealm), true);

  // Create the global system statistics process.
  spawn(new System(), true);

  // Create the global HTTP authentication router.
  authenticator_manager = new AuthenticatorManager();

  // Create the global reaper process.
  process::internal::reaper =
    spawn(new process::internal::ReaperProcess(), true);

  // Create the global job object manager process.
#ifdef __WINDOWS__
  process::internal::job_object_manager =
    spawn(new process::internal::JobObjectManager(), true);
#endif // __WINDOWS__

  // Initialize the mime types.
  mime::initialize();

  // Add a route for getting process information.
  lambda::function<Future<Response>(const Request&)> __processes__ =
    lambda::bind(&ProcessManager::__processes__, process_manager, lambda::_1);

  processes_route = new Route("/__processes__", None(), __processes__);

  VLOG(1) << "libprocess is initialized on " << address() << " with "
          << num_worker_threads << " worker threads";

  // Return `true` to indicate that this was the first invocation of
  // `process::initialize()`.
  return true;
}


// Gracefully winds down libprocess in roughly the reverse order of
// initialization.
//
// NOTE: `finalize_wsa` controls whether libprocess also finalizes
// the Windows socket stack, which affects the entire process.
void finalize(bool finalize_wsa)
{
  // The clock is only paused during tests.  Pausing may lead to infinite
  // waits during clean up, so we make sure the clock is running normally.
  Clock::resume();

  // This will terminate the underlying process for the `Route`.
  delete processes_route;
  processes_route = nullptr;

  // Close the server socket.
  // This will prevent any further connections managed by the `SocketManager`.
  synchronized (socket_mutex) {
    // Explicitly terminate the callback loop used to accept incoming
    // connections. This is necessary as the server socket ignores
    // most errors, including when the server socket has been closed.
    future_accept.discard();

    delete __s__;
    __s__ = nullptr;
  }

  // Terminate all running processes and prevent further processes from
  // being spawned. This will also clean up any metadata for running
  // processes held by the `SocketManager`. After this method returns,
  // libprocess should be single-threaded.
  process_manager->finalize();

  // Now that all threads except for the main thread have joined, we should
  // delete the one remaining `_executor_` pointer.
  delete _executor_;
  _executor_ = nullptr;

  // This clears any remaining timers. Because the event loop has been
  // stopped, no timers will fire.
  Clock::finalize();

  // Clean up the socket manager.
  // Terminating processes above will also clean up any links between
  // processes (which may be expressed as open sockets) and the various
  // `HttpProxy` processes managing incoming HTTP requests. We cannot
  // delete the `SocketManager` yet, since the `ProcessManager` may
  // potentially dereference it.
  socket_manager->finalize();

  // This is dereferenced inside `ProcessBase::visit(HttpEvent&)`.
  // We can safely delete it since no further incoming HTTP connections
  // can be made because the server socket has been destroyed. This must
  // be deleted before the `ProcessManager` as it will indirectly
  // dereference the `ProcessManager`.
  delete authenticator_manager;
  authenticator_manager = nullptr;

  // At this point, there should be no running processes, no sockets,
  // and a single remaining thread. We can safely remove the global
  // `SocketManager` and `ProcessManager` pointers now.
  delete socket_manager;
  socket_manager = nullptr;

  delete process_manager;
  process_manager = nullptr;

  // Clear the public address of the server socket.
  // NOTE: This variable is necessary for process communication, so it
  // cannot be cleared until after the `ProcessManager` is deleted.
  __address__ = inet4::Address::ANY_ANY();

  // Reset any IPv6 addresses set on the process.
  __address6__ = None();

  // Finally, reset the Flags to defaults.
  *libprocess_flags = internal::Flags();

#ifdef __WINDOWS__
  if (finalize_wsa && !net::wsa_cleanup()) {
    EXIT(EXIT_FAILURE) << "Failed to finalize the WSA socket stack";
  }
#endif // __WINDOWS__
}


string absolutePath(const string& path)
{
  process::initialize();

  return process_manager->absolutePath(path);
}


Address address()
{
  process::initialize();
  return __address__;
}


PID<Logging> logging()
{
  process::initialize();
  return _logging;
}


long workers()
{
  process::initialize();
  return process_manager->workers();
}


HttpProxy::HttpProxy(const Socket& _socket)
  : ProcessBase(ID::generate("__http__")),
    socket(_socket) {}


void HttpProxy::finalize()
{
  // Need to make sure response producers know not to continue to
  // create a response (streaming or otherwise).
  if (pipe.isSome()) {
    http::Pipe::Reader reader = pipe.get();
    reader.close();
  }
  pipe = None();

  while (!items.empty()) {
    Item* item = items.front();

    // Attempt to discard the future.
    item->future.discard();

    // But it might have already been ready. In general, we need to
    // wait until this future is potentially ready in order to attempt
    // to close a pipe if one exists.
    item->future.onReady([](const Response& response) {
      // Cleaning up a response (i.e., closing any open Pipes in the
      // event Response::type is PIPE).
      if (response.type == Response::PIPE) {
        CHECK_SOME(response.reader);
        http::Pipe::Reader reader = response.reader.get(); // Remove const.
        reader.close();
      }
    });

    items.pop();
    delete item;
  }

  // Just in case this process gets killed outside of `SocketManager::close`,
  // remove the proxy from the socket.
  socket_manager->unproxy(socket);
}


void HttpProxy::enqueue(const Response& response, const Request& request)
{
  handle(Future<Response>(response), request);
}


void HttpProxy::handle(const Future<Response>& future, const Request& request)
{
  items.push(new Item(request, future));

  if (items.size() == 1) {
    next();
  }
}


void HttpProxy::next()
{
  if (items.size() > 0) {
    // Wait for any transition of the future.
    items.front()->future.onAny(
        defer(self(), &HttpProxy::waited, lambda::_1));
  }
}


void HttpProxy::waited(const Future<Response>& future)
{
  CHECK(items.size() > 0);
  Item* item = items.front();

  CHECK(future == item->future);

  // Process the item and determine if we're done or not (so we know
  // whether to start waiting on the next responses).
  bool processed = process(item->future, item->request);

  items.pop();
  delete item;

  if (processed) {
    next();
  }
}


bool HttpProxy::process(const Future<Response>& future, const Request& request)
{
  if (!future.isReady()) {
    // TODO(benh): Consider handling other "states" of future
    // (discarded, failed, etc) with different HTTP statuses.
    Response response = future.isFailed()
      ? InternalServerError(future.failure())
      : InternalServerError("discarded future");

    VLOG(1) << "Returning '" << response.status << "'"
            << " for '" << request.url.path << "'"
            << " ("
            << (future.isFailed()
                  ? future.failure()
                  : "discarded") << ")";

    socket_manager->send(response, request, socket);

    return true; // All done, can process next response.
  }

  Response response = future.get();

  // If the response specifies a path, try and perform a sendfile.
  if (response.type == Response::PATH) {
    // Make sure no body is sent (this is really an error and
    // should be reported and no response sent.
    response.body.clear();

    const string& path = response.path;
    int_fd fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errno == ENOENT || errno == ENOTDIR) {
          VLOG(1) << "Returning '404 Not Found' for path '" << path << "'";
          socket_manager->send(NotFound(), request, socket);
      } else {
        const string error = os::strerror(errno);
        VLOG(1) << "Failed to send file at '" << path << "': " << error;
        socket_manager->send(InternalServerError(), request, socket);
      }
    } else {
      struct stat s; // Need 'struct' because of function named 'stat'.
      // We don't bother introducing a `os::fstat` since this is only
      // one of two places where we use `fstat` in the entire codebase
      // as of writing this comment.
#ifdef __WINDOWS__
      if (::fstat(fd.crt(), &s) != 0) {
#else
      if (::fstat(fd, &s) != 0) {
#endif
        const string error = os::strerror(errno);
        VLOG(1) << "Failed to send file at '" << path << "': " << error;
        socket_manager->send(InternalServerError(), request, socket);
      } else if (S_ISDIR(s.st_mode)) {
        VLOG(1) << "Returning '404 Not Found' for directory '" << path << "'";
        socket_manager->send(NotFound(), request, socket);
      } else {
        // While the user is expected to properly set a 'Content-Type'
        // header, we fill in (or overwrite) 'Content-Length' header.
        stringstream out;
        out << s.st_size;
        response.headers["Content-Length"] = out.str();

        if (s.st_size == 0) {
          socket_manager->send(response, request, socket);
          return true; // All done, can process next request.
        }

        VLOG(1) << "Sending file at '" << path << "' with length " << s.st_size;

        // TODO(benh): Consider a way to have the socket manager turn
        // on TCP_CORK for both sends and then turn it off.
        socket_manager->send(
            new HttpResponseEncoder(response, request),
            true,
            socket);

        // Note the file descriptor gets closed by FileEncoder.
        socket_manager->send(
            new FileEncoder(fd, s.st_size),
            request.keepAlive,
            socket);
      }
    }
  } else if (response.type == Response::PIPE) {
    // Make sure no body is sent (this is really an error and
    // should be reported and no response sent.
    response.body.clear();

    // While the user is expected to properly set a 'Content-Type'
    // header, we fill in (or overwrite) 'Transfer-Encoding' header.
    response.headers["Transfer-Encoding"] = "chunked";

    VLOG(3) << "Starting \"chunked\" streaming";

    socket_manager->send(
        new HttpResponseEncoder(response, request),
        true,
        socket);

    CHECK_SOME(response.reader);
    http::Pipe::Reader reader = response.reader.get();

    pipe = reader;

    // Avoid copying the request for each chunk read on the pipe.
    //
    // TODO(bmahler): Make request a process::Owned or
    // process::Shared from the point where it is decoded.
    Owned<Request> request_(new Request(request));

    reader.read()
      .onAny(defer(self(), &Self::stream, request_, lambda::_1));

    return false; // Streaming, don't process next response (yet)!
  } else {
    socket_manager->send(response, request, socket);
  }

  return true; // All done, can process next response.
}


void HttpProxy::stream(
    const Owned<Request>& request,
    const Future<string>& chunk)
{
  CHECK_SOME(pipe);
  CHECK_NOTNULL(request.get());

  http::Pipe::Reader reader = pipe.get();

  bool finished = false; // Whether we're done streaming.

  if (chunk.isReady()) {
    std::ostringstream out;

    if (chunk.get().empty()) {
      // Finished reading.
      out << "0\r\n" << "\r\n";
      finished = true;
    } else {
      out << std::hex << chunk.get().size() << "\r\n";
      out << chunk.get();
      out << "\r\n";

      // Keep reading.
      reader.read()
        .onAny(defer(self(), &Self::stream, request, lambda::_1));
    }

    // Always persist the connection when streaming is not finished.
    socket_manager->send(
        new DataEncoder(out.str()),
        finished ? request->keepAlive : true,
        socket);
  } else if (chunk.isFailed()) {
    VLOG(1) << "Failed to read from stream: " << chunk.failure();
    // TODO(bmahler): Have to close connection if headers were sent!
    socket_manager->send(InternalServerError(), *request, socket);
    finished = true;
  } else {
    VLOG(1) << "Failed to read from stream: discarded";
    // TODO(bmahler): Have to close connection if headers were sent!
    socket_manager->send(InternalServerError(), *request, socket);
    finished = true;
  }

  if (finished) {
    reader.close();
    pipe = None();
    next();
  }
}


SocketManager::SocketManager() {}


SocketManager::~SocketManager() {}


void SocketManager::finalize()
{
  // We require the `SocketManager` to be finalized after the server socket
  // has been closed. This means that no further incoming sockets will be
  // given to the `SocketManager` at this point.
  CHECK(__s__ == nullptr);

  // We require all processes to be terminated prior to finalizing the
  // `SocketManager`. This simplifies the finalization logic as we do not
  // have to worry about sockets or links being created during cleanup.
  CHECK(gc == nullptr);

  int_fd socket = -1;
  // Close each socket.
  // Don't hold the lock since there is a dependency between `SocketManager`
  // and `ProcessManager`, which may result in deadlock.  See comments in
  // `SocketManager::close` for more details.
  do {
    synchronized (mutex) {
      socket = !sockets.empty() ? sockets.begin()->first : -1;
    }

    if (socket >= 0) {
      // This will also clean up any other state related to this socket.
      close(socket);
    }
  } while (socket >= 0);
}


void SocketManager::accepted(const Socket& socket)
{
  synchronized (mutex) {
    CHECK(sockets.count(socket) == 0);
    sockets.emplace(socket, socket);
  }
}


namespace internal {

void ignore_recv_data(
    const Future<size_t>& length,
    Socket socket,
    char* data,
    size_t size)
{
  if (length.isDiscarded() || length.isFailed()) {
    socket_manager->close(socket);
    delete[] data;
    return;
  }

  if (length.get() == 0) {
    socket_manager->close(socket);
    delete[] data;
    return;
  }

  socket.recv(data, size)
    .onAny(lambda::bind(&ignore_recv_data, lambda::_1, socket, data, size));
}


// Forward declaration.
void send(Encoder* encoder, Socket socket);


} // namespace internal {


void SocketManager::link_connect(
    const Future<Nothing>& future,
    Socket socket,
    const UPID& to)
{
  if (future.isDiscarded() || future.isFailed()) {
    if (future.isFailed()) {
      VLOG(1) << "Failed to link, connect: " << future.failure();
    }

    // Check if SSL is enabled, and whether we allow a downgrade to
    // non-SSL traffic.
#ifdef USE_SSL_SOCKET
    bool attempt_downgrade =
      future.isFailed() &&
      network::openssl::flags().enabled &&
      network::openssl::flags().support_downgrade &&
      socket.kind() == SocketImpl::Kind::SSL;

    Option<Socket> poll_socket = None();

    // If we allow downgrading from SSL to non-SSL, then retry as a
    // POLL socket.
    if (attempt_downgrade) {
      synchronized (mutex) {
        // It is possible that a prior call to `link()` with `RECONNECT`
        // semantics has swapped out this socket before we finished
        // connecting. In this case, we simply stop here and allow the
        // latest created socket to complete the link.
        if (sockets.count(socket) <= 0) {
          return;
        }

        Try<Socket> create = Socket::create(SocketImpl::Kind::POLL);
        if (create.isError()) {
          VLOG(1) << "Failed to link, create socket: " << create.error();
          socket_manager->close(socket);
          return;
        }

        poll_socket = create.get();

        // Update all the data structures that are mapped to the socket
        // that just failed to connect. They will now point to the new
        // POLL socket we are about to try to connect. Even if the
        // process has exited, persistent links will stay around, and
        // temporary links will get cleaned up as they would otherwise.
        swap_implementing_socket(socket, poll_socket.get());
      }

      CHECK_SOME(poll_socket);
      poll_socket->connect(to.address)
        .onAny(lambda::bind(
            &SocketManager::link_connect,
            this,
            lambda::_1,
            poll_socket.get(),
            to));

      // We don't need to 'shutdown()' the socket as it was never
      // connected.
      return;
    }
#endif

    socket_manager->close(socket);
    return;
  }

  synchronized (mutex) {
    // It is possible that a prior call to `link()` with `RECONNECT`
    // semantics has swapped out this socket before we finished
    // connecting. In this case, we simply stop here and allow the
    // latest created socket to complete the link.
    if (sockets.count(socket) <= 0) {
      return;
    }

    size_t size = 80 * 1024;
    char* data = new char[size];

    socket.recv(data, size)
      .onAny(lambda::bind(
          &internal::ignore_recv_data,
          lambda::_1,
          socket,
          data,
          size));
  }

  // In order to avoid a race condition where internal::send() is
  // called after SocketManager::link() but before the socket is
  // connected, we initialize the 'outgoing' queue in
  // SocketManager::link() and then check if the queue has anything in
  // it to send during this connection completion. When a subsequent
  // call to SocketManager::send() occurs we'll now just add the
  // encoder to the 'outgoing' queue, and when we complete the
  // connection here we'll start sending, otherwise when we call
  // SocketManager::next() the 'outgoing' queue will get removed and
  // any subsequent call to SocketManager::send() will take care of
  // setting it back up and sending.
  Encoder* encoder = socket_manager->next(socket);

  if (encoder != nullptr) {
    internal::send(encoder, socket);
  }
}


void SocketManager::link(
    ProcessBase* process,
    const UPID& to,
    const ProcessBase::RemoteConnection remote,
    const SocketImpl::Kind& kind)
{
  // TODO(benh): The semantics we want to support for link are such
  // that if there is nobody to link to (local or remote) then an
  // ExitedEvent gets generated. This functionality has only been
  // implemented when the link is local, not remote. Of course, if
  // there is nobody listening on the remote side, then this should
  // work remotely ... but if there is someone listening remotely just
  // not at that id, then it will silently continue executing.

  CHECK_NOTNULL(process);

  Option<Socket> socket = None();
  bool connect = false;

  synchronized (mutex) {
    // Check if the socket address is remote.
    if (to.address != __address__) {
      // Check if there isn't already a persistent link.
      if (persists.count(to.address) == 0) {
        // Okay, no link, let's create a socket.
        // The kind of socket we create is passed in as an argument.
        // This allows us to support downgrading the connection type
        // from SSL to POLL if enabled.
        Try<Socket> create = Socket::create(kind);
        if (create.isError()) {
          LOG(WARNING) << "Failed to link, create socket: " << create.error();

          // Failure to create a new socket should generate an `ExitedEvent`
          // for the linkee. At this point, we have not passed ownership of
          // this socket to the `SocketManager`, so there is only one possible
          // linkee to notify.
          process->enqueue(new ExitedEvent(to));
          return;
        }
        socket = create.get();
        int_fd s = socket.get().get();

        CHECK(sockets.count(s) == 0);
        sockets.emplace(s, socket.get());

        addresses.emplace(s, to.address);

        persists.emplace(to.address, s);

        // Initialize 'outgoing' to prevent a race with
        // SocketManager::send() while the socket is not yet connected.
        // Initializing the 'outgoing' queue prevents
        // SocketManager::send() from trying to write before it's
        // connected.
        outgoing[s];

        connect = true;
      } else if (remote == ProcessBase::RemoteConnection::RECONNECT) {
        // There is a persistent link already and the linker wants to
        // create a new socket anyway.
        Try<Socket> create = Socket::create(kind);
        if (create.isError()) {
          LOG(WARNING) << "Failed to link, create socket: " << create.error();

          // Failure to create a new socket should generate an `ExitedEvent`
          // for the linkee. At this point, we have not passed ownership of
          // this socket to the `SocketManager`, so there is only one possible
          // linkee to notify.
          process->enqueue(new ExitedEvent(to));
          return;
        }

        socket = create.get();

        // Update all the data structures that are mapped to the old
        // socket. They will now point to the new socket we are about
        // to try to connect.
        Socket existing(sockets.at(persists.at(to.address)));
        swap_implementing_socket(existing, socket.get());

        // The `existing` socket could be a perfectly functional socket.
        // In this case, the socket may be referenced in the callback
        // loop of `internal::ignore_recv_data`. We shutdown the socket
        // in order to interrupt this callback loop and thereby release
        // the final socket reference. This will not result in an
        // `ExitedEvent` because we have already removed the `existing`
        // socket from the mapping of linkees and linkers.
        Try<Nothing> shutdown = existing.shutdown();
        if (shutdown.isError()) {
          VLOG(1) << "Failed to shutdown old link: " << shutdown.error();
        }

        connect = true;
      }
    }

    links.linkers[to].insert(process);
    links.linkees[process].insert(to);
    if (to.address != __address__) {
      links.remotes[to.address].insert(to);
    }
  }

  if (connect) {
    CHECK_SOME(socket);
    socket->connect(to.address)
      .onAny(lambda::bind(
          &SocketManager::link_connect,
          this,
          lambda::_1,
          socket.get(),
          to));
  }
}


// Tests can declare this function and use it to fetch the socket FD's
// for links managed by the `SocketManager`. Without explicitly
// declaring this function, it is not visible. This is the preferred
// behavior as we do not want applications to have easy access to
// managed FD's.
Option<int_fd> get_persistent_socket(const UPID& to)
{
  return socket_manager->get_persistent_socket(to);
}


Option<int_fd> SocketManager::get_persistent_socket(const UPID& to)
{
  synchronized (mutex) {
    if (persists.count(to.address) > 0) {
      return persists.at(to.address);
    }
  }

  return None();
}


PID<HttpProxy> SocketManager::proxy(const Socket& socket)
{
  HttpProxy* proxy = nullptr;

  synchronized (mutex) {
    // This socket might have been asked to get closed (e.g., remote
    // side hang up) while a process is attempting to handle an HTTP
    // request. Thus, if there is no more socket, return an empty PID.
    if (sockets.count(socket) > 0) {
      if (proxies.count(socket) > 0) {
        return proxies[socket]->self();
      } else {
        proxy = new HttpProxy(sockets.at(socket));
        proxies[socket] = proxy;
      }
    }
  }

  // Now check if we need to spawn a newly created proxy. Note that we
  // need to do this outside of the synchronized block above to avoid
  // a possible deadlock (because spawn eventually synchronizes on
  // ProcessManager and ProcessManager::cleanup synchronizes on
  // ProcessManager and then SocketManager, so a deadlock results if
  // we do spawn within the synchronized block above).
  if (proxy != nullptr) {
    return spawn(proxy, true);
  }

  return PID<HttpProxy>();
}


void SocketManager::unproxy(const Socket& socket)
{
  synchronized (mutex) {
    auto proxy = proxies.find(socket);

    // NOTE: We may have already removed this proxy if the associated
    // `HttpProxy` was destructed via `SocketManager::close`.
    if (proxy != proxies.end()) {
      proxies.erase(proxy);
    }
  }
}


namespace internal {

void _send(
    const Future<size_t>& result,
    Socket socket,
    Encoder* encoder,
    size_t size);


void send(Encoder* encoder, Socket socket)
{
  switch (encoder->kind()) {
    case Encoder::DATA: {
      size_t size;
      const char* data = static_cast<DataEncoder*>(encoder)->next(&size);
      socket.send(data, size)
        .onAny(lambda::bind(
            &internal::_send,
            lambda::_1,
            socket,
            encoder,
            size));
      break;
    }
    case Encoder::FILE: {
      off_t offset;
      size_t size;
      int_fd fd = static_cast<FileEncoder*>(encoder)->next(&offset, &size);
      socket.sendfile(fd, offset, size)
        .onAny(lambda::bind(
            &internal::_send,
            lambda::_1,
            socket,
            encoder,
            size));
      break;
    }
  }
}


void _send(
    const Future<size_t>& length,
    Socket socket,
    Encoder* encoder,
    size_t size)
{
  if (length.isDiscarded() || length.isFailed()) {
    socket_manager->close(socket);
    delete encoder;
  } else {
    // Update the encoder with the amount sent.
    encoder->backup(size - length.get());

    // See if there is any more of the message to send.
    if (encoder->remaining() == 0) {
      delete encoder;

      // Check for more stuff to send on socket.
      Encoder* next = socket_manager->next(socket);
      if (next != nullptr) {
        send(next, socket);
      }
    } else {
      send(encoder, socket);
    }
  }
}

} // namespace internal {


void SocketManager::send(Encoder* encoder, bool persist, const Socket& socket)
{
  CHECK(encoder != nullptr);

  synchronized (mutex) {
    if (sockets.count(socket) > 0) {
      // Update whether or not this socket should get disposed after
      // there is no more data to send.
      if (!persist) {
        dispose.insert(socket);
      }

      if (outgoing.count(socket) > 0) {
        outgoing[socket].push(encoder);
        encoder = nullptr;
      } else {
        // Initialize the outgoing queue.
        outgoing[socket];
      }
    } else {
      VLOG(1) << "Attempting to send on a no longer valid socket!";
      delete encoder;
      encoder = nullptr;
    }
  }

  if (encoder != nullptr) {
    internal::send(encoder, socket);
  }
}


void SocketManager::send(
    const Response& response,
    const Request& request,
    const Socket& socket)
{
  bool persist = request.keepAlive;

  // Don't persist the connection if the headers include
  // 'Connection: close'.
  if (response.headers.contains("Connection")) {
    if (response.headers.get("Connection").get() == "close") {
      persist = false;
    }
  }

  send(new HttpResponseEncoder(response, request), persist, socket);
}


void SocketManager::send_connect(
    const Future<Nothing>& future,
    Socket socket,
    Message&& message)
{
  if (future.isDiscarded() || future.isFailed()) {
    if (future.isFailed()) {
      VLOG(1) << "Failed to send '" << message.name << "' to '"
              << message.to.address << "', connect: " << future.failure();
    }

    // Check if SSL is enabled, and whether we allow a downgrade to
    // non-SSL traffic.
#ifdef USE_SSL_SOCKET
    bool attempt_downgrade =
      future.isFailed() &&
      network::openssl::flags().enabled &&
      network::openssl::flags().support_downgrade &&
      socket.kind() == SocketImpl::Kind::SSL;

    Option<Socket> poll_socket = None();

    // If we allow downgrading from SSL to non-SSL, then retry as a
    // POLL socket.
    if (attempt_downgrade) {
      synchronized (mutex) {
        Try<Socket> create = Socket::create(SocketImpl::Kind::POLL);
        if (create.isError()) {
          VLOG(1) << "Failed to link, create socket: " << create.error();
          socket_manager->close(socket);
          return;
        }

        poll_socket = create.get();

        // Update all the data structures that are mapped to the socket
        // that just failed to connect. They will now point to the new
        // POLL socket we are about to try to connect. Even if the
        // process has exited, persistent links will stay around, and
        // temporary links will get cleaned up as they would otherwise.
        swap_implementing_socket(socket, poll_socket.get());
      }

      CHECK_SOME(poll_socket);
      poll_socket.get().connect(message.to.address)
        .onAny(lambda::bind(
            // TODO(benh): with C++14 we can use lambda instead of
            // `std::bind` and capture `message` with a `std::move`.
            [this, poll_socket](Message& message, const Future<Nothing>& f) {
              send_connect(f, poll_socket.get(), std::move(message));
            }, std::move(message), lambda::_1));

      // We don't need to 'shutdown()' the socket as it was never
      // connected.
      return;
    }
#endif

    socket_manager->close(socket);

    return;
  }

  Encoder* encoder = new MessageEncoder(message);

  // Receive and ignore data from this socket. Note that we don't
  // expect to receive anything other than HTTP '202 Accepted'
  // responses which we just ignore.
  size_t size = 80 * 1024;
  char* data = new char[size];

  socket.recv(data, size)
    .onAny(lambda::bind(
        &internal::ignore_recv_data,
        lambda::_1,
        socket,
        data,
        size));

  internal::send(encoder, socket);
}


void SocketManager::send(Message&& message, const SocketImpl::Kind& kind)
{
  const Address& address = message.to.address;

  Option<Socket> socket = None();
  bool connect = false;

  synchronized (mutex) {
    // Check if there is already a socket.
    bool persist = persists.count(address) > 0;
    bool temp = temps.count(address) > 0;
    if (persist || temp) {
      int_fd s = persist ? persists[address] : temps[address];
      CHECK(sockets.count(s) > 0);
      socket = sockets.at(s);

      // Update whether or not this socket should get disposed after
      // there is no more data to send.
      if (!persist) {
        dispose.insert(socket.get());
      }

      if (outgoing.count(socket.get()) > 0) {
        outgoing[socket.get()].push(new MessageEncoder(message));
        return;
      } else {
        // Initialize the outgoing queue.
        outgoing[socket.get()];
      }

    } else {
      // No persistent or temporary socket to the socket address
      // currently exists, so we create a temporary one.
      // The kind of socket we create is passed in as an argument.
      // This allows us to support downgrading the connection type
      // from SSL to POLL if enabled.
      Try<Socket> create = Socket::create(kind);
      if (create.isError()) {
        VLOG(1) << "Failed to send, create socket: " << create.error();
        return;
      }
      socket = create.get();
      int_fd s = socket.get();

      CHECK(sockets.count(s) == 0);
      sockets.emplace(s, socket.get());

      addresses.emplace(s, address);
      temps.emplace(address, s);

      dispose.insert(s);

      // Initialize the outgoing queue.
      outgoing[s];

      connect = true;
    }
  }

  if (connect) {
    CHECK_SOME(socket);
    socket->connect(address)
      .onAny(lambda::bind(
            // TODO(benh): with C++14 we can use lambda instead of
            // `std::bind` and capture `message` with a `std::move`.
            [this, socket](Message& message, const Future<Nothing>& f) {
              send_connect(f, socket.get(), std::move(message));
            }, std::move(message), lambda::_1));
  } else {
    // If we're not connecting and we haven't added the encoder to
    // the 'outgoing' queue then schedule it to be sent.
    internal::send(new MessageEncoder(message), socket.get());
  }
}


Encoder* SocketManager::next(int_fd s)
{
  HttpProxy* proxy = nullptr; // Non-null if needs to be terminated.

  synchronized (mutex) {
    // We cannot assume 'sockets.count(s) > 0' here because it's
    // possible that 's' has been removed with a call to
    // SocketManager::close. For example, it could be the case that a
    // socket has gone to CLOSE_WAIT and the call to read in
    // io::read returned 0 causing SocketManager::close to get
    // invoked. Later a call to 'send' or 'sendfile' (e.g., in
    // send_data or send_file) can "succeed" (because the socket is
    // not "closed" yet because there are still some Socket
    // references, namely the reference being used in send_data or
    // send_file!). However, when SocketManager::next is actually
    // invoked we find out there there is no more data and thus stop
    // sending.
    // TODO(benh): Should we actually finish sending the data!?
    if (sockets.count(s) > 0) {
      CHECK(outgoing.count(s) > 0);

      if (!outgoing[s].empty()) {
        // More messages!
        Encoder* encoder = outgoing[s].front();
        outgoing[s].pop();
        return encoder;
      } else {
        // No more messages ... erase the outgoing queue.
        outgoing.erase(s);

        if (dispose.count(s) > 0) {
          // This is either a temporary socket we created or it's a
          // socket that we were receiving data from and possibly
          // sending HTTP responses back on. Clean up either way.
          Option<Address> address = addresses.get(s);
          if (address.isSome()) {
            CHECK(temps.count(address.get()) > 0 && temps[address.get()] == s);
            temps.erase(address.get());
            addresses.erase(s);
          }

          if (proxies.count(s) > 0) {
            proxy = proxies[s];
            proxies.erase(s);
          }

          dispose.erase(s);

          auto iterator = sockets.find(s);

          // We don't actually close the socket (we wait for the Socket
          // abstraction to close it once there are no more references),
          // but we do shutdown the receiving end so any DataDecoder
          // will get cleaned up (which might have the last reference).

          // Hold on to the Socket and remove it from the 'sockets'
          // map so that in the case where 'shutdown()' ends up
          // calling close the termination logic is not run twice.
          Socket socket = iterator->second;
          sockets.erase(iterator);

          Try<Nothing> shutdown = socket.shutdown();
          if (shutdown.isError()) {
            LOG(ERROR) << "Failed to shutdown socket with fd " << socket.get()
                       << ", address " << (socket.address().isSome()
                                             ? stringify(socket.address().get())
                                             : "N/A")
                       << ": " << shutdown.error();
          }
        }
      }
    }
  }

  // We terminate the proxy outside the synchronized block to avoid
  // possible deadlock between the ProcessManager and SocketManager
  // (see comment in SocketManager::proxy for more information).
  if (proxy != nullptr) {
    terminate(proxy);
  }

  return nullptr;
}


void SocketManager::close(int_fd s)
{
  Option<UPID> proxy; // Some if an `HttpProxy` needs to be terminated.

  synchronized (mutex) {
    // This socket might not be active if it was already asked to get
    // closed (e.g., a write on the socket failed so we try and close
    // it and then later the recv side of the socket gets closed so we
    // try and close it again). Thus, ignore the request if we don't
    // know about the socket.
    if (sockets.count(s) > 0) {
      // Clean up any remaining encoders for this socket.
      if (outgoing.count(s) > 0) {
        while (!outgoing[s].empty()) {
          Encoder* encoder = outgoing[s].front();
          delete encoder;
          outgoing[s].pop();
        }

        outgoing.erase(s);
      }

      // Clean up after sockets used for remote communication.
      Option<Address> address = addresses.get(s);
      if (address.isSome()) {
        // Don't bother invoking `exited` unless socket was persistent.
        if (persists.count(address.get()) > 0 && persists[address.get()] == s) {
          persists.erase(address.get());
          exited(address.get()); // Generate ExitedEvent(s)!
        } else if (temps.count(address.get()) > 0 &&
                   temps[address.get()] == s) {
          temps.erase(address.get());
        }

        addresses.erase(s);
      }

      // Clean up any proxy associated with this socket.
      if (proxies.count(s) > 0) {
        proxy = proxies.at(s)->self();
        proxies.erase(s);
      }

      dispose.erase(s);
      auto iterator = sockets.find(s);

      // We need to stop any 'ignore_data' receivers as they may have
      // the last Socket reference so we shutdown recvs but don't do a
      // full close (since that will be taken care of by ~Socket, see
      // comment below). Calling 'shutdown' will trigger 'ignore_data'
      // which will get back a 0 (i.e., EOF) when it tries to 'recv'
      // from the socket. Note we need to do this before we call
      // 'sockets.erase(s)' to avoid the potential race with the last
      // reference being in 'sockets'.

      // Hold on to the Socket and remove it from the 'sockets' map so
      // that in the case where 'shutdown()' ends up calling close the
      // termination logic is not run twice.
      Socket socket = iterator->second;
      sockets.erase(iterator);

      Try<Nothing> shutdown = socket.shutdown();
      if (shutdown.isError()) {
        LOG(ERROR) << "Failed to shutdown socket with fd " << socket.get()
                   << ", address " << (socket.address().isSome()
                                         ? stringify(socket.address().get())
                                         : "N/A")
                   << ": " << shutdown.error();
      }
    }
  }

  // We terminate the proxy outside the synchronized block to avoid
  // possible deadlock between the ProcessManager and SocketManager.
  if (proxy.isSome()) {
    terminate(proxy.get());
  }

  // Note that we don't actually:
  //
  //   close(s);
  //
  // Because, for example, there could be a race between an HttpProxy
  // trying to do send a response with SocketManager::send() or a
  // process might be responding to another Request (e.g., trying
  // to do a sendfile) since these things may be happening
  // asynchronously we can't close the socket yet, because it might
  // get reused before any of the above things have finished, and then
  // we'll end up sending data on the wrong socket! Instead, we rely
  // on the last reference of our Socket object to close the
  // socket. Note, however, that since socket is no longer in
  // 'sockets' any attempt to send with it will just get ignored.
  // TODO(benh): Always do a 'shutdown(s, SHUT_RDWR)' since that
  // should keep the file descriptor valid until the last Socket
  // reference does a close but force all event loop watchers to stop?
}


void SocketManager::exited(const Address& address)
{
  // TODO(benh): It would be cleaner if this routine could call back
  // into ProcessManager ... then we wouldn't have to convince
  // ourselves that the accesses to each Process object will always be
  // valid.
  synchronized (mutex) {
    if (!links.remotes.contains(address)) {
      return; // No linkees for this socket address!
    }

    foreach (const UPID& linkee, links.remotes[address]) {
      // Find and notify the linkers.
      CHECK(links.linkers.contains(linkee));

      foreach (ProcessBase* linker, links.linkers[linkee]) {
        linker->enqueue(new ExitedEvent(linkee));

        // Remove the linkee pid from the linker.
        CHECK(links.linkees.contains(linker));

        links.linkees[linker].erase(linkee);
        if (links.linkees[linker].empty()) {
          links.linkees.erase(linker);
        }
      }

      links.linkers.erase(linkee);
    }

    links.remotes.erase(address);
  }
}


void SocketManager::exited(ProcessBase* process)
{
  // An exited event is enough to cause the process to get deleted
  // (e.g., by the garbage collector), which means we can't
  // dereference process (or even use the address) after we enqueue at
  // least one exited event. Thus, we save the process pid.
  const UPID pid = process->pid;

  // Likewise, we need to save the current time of the process so we
  // can update the clocks of linked processes as appropriate.
  const Time time = Clock::now(process);

  synchronized (mutex) {
    // If this process had linked to anything, we need to clean
    // up any pointers to it. Also, if this process was the last
    // linker to a remote linkee, we must remove linkee from the
    // remotes!
    if (links.linkees.contains(process)) {
      foreach (const UPID& linkee, links.linkees[process]) {
        CHECK(links.linkers.contains(linkee));

        links.linkers[linkee].erase(process);
        if (links.linkers[linkee].empty()) {
          links.linkers.erase(linkee);

          // The exited process was the last linker for this linkee,
          // so we need to remove the linkee from the remotes.
          if (linkee.address != __address__) {
            CHECK(links.remotes.contains(linkee.address));

            links.remotes[linkee.address].erase(linkee);
            if (links.remotes[linkee.address].empty()) {
              links.remotes.erase(linkee.address);
            }
          }
        }
      }
      links.linkees.erase(process);
    }

    // Find the linkers to notify.
    if (!links.linkers.contains(pid)) {
      return; // No linkers for this process!
    }

    foreach (ProcessBase* linker, links.linkers[pid]) {
      CHECK(linker != process) << "Process linked with itself";
      Clock::update(linker, time);
      linker->enqueue(new ExitedEvent(pid));

      // Remove the linkee pid from the linker.
      CHECK(links.linkees.contains(linker));

      links.linkees[linker].erase(pid);
      if (links.linkees[linker].empty()) {
        links.linkees.erase(linker);
      }
    }

    links.linkers.erase(pid);
  }
}


void SocketManager::swap_implementing_socket(
    const Socket& from, const Socket& to)
{
  int_fd from_fd = from.get();
  int_fd to_fd = to.get();

  synchronized (mutex) {
    // Make sure 'from' and 'to' are valid to swap.
    CHECK(sockets.count(from_fd) > 0);
    CHECK(sockets.count(to_fd) == 0);

    sockets.erase(from_fd);
    sockets.emplace(to_fd, to);

    // Update the dispose set if this is a temporary link.
    if (dispose.count(from_fd) > 0) {
      dispose.insert(to_fd);
      dispose.erase(from_fd);
    }

    // Update the fd that this address is associated with. Once we've
    // done this we can update the 'temps' and 'persists'
    // data structures using this updated address.
    Option<Address> address = addresses.get(from_fd);
    CHECK_SOME(address);
    addresses.emplace(to_fd, address.get());
    addresses.erase(from_fd);

    // If this address is a persistent or temporary link
    // that matches the original FD.
    if (persists.count(address.get()) > 0 &&
        persists.at(address.get()) == from_fd) {
      persists[address.get()] = to_fd;
      // No need to erase as we're changing the value, not the key.
    } else if (temps.count(address.get()) > 0 &&
               temps.at(address.get()) == from_fd) {
      temps[address.get()] = to_fd;
      // No need to erase as we're changing the value, not the key.
    }

    // Move any encoders queued against this link to the new socket.
    outgoing[to_fd] = std::move(outgoing[from_fd]);
    outgoing.erase(from_fd);

    // Update the fd any proxies are associated with.
    if (proxies.count(from_fd) > 0) {
      proxies[to_fd] = proxies[from_fd];
      proxies.erase(from_fd);
    }
  }
}


ProcessManager::ProcessManager(const Option<string>& _delegate)
  : delegate(_delegate),
    running(0),
    joining_threads(false),
    finalizing(false) {}


ProcessManager::~ProcessManager() {}


void ProcessManager::finalize()
{
  CHECK(gc != nullptr);

  // Prevent anymore processes from being spawned.
  finalizing.store(true);

  // Terminate one process at a time. Events are deleted and the process
  // is erased from `processes` in ProcessManager::cleanup(). Don't hold
  // the lock or process the whole map as terminating one process might
  // trigger other terminations.
  //
  // We skip the GC process in this loop and instead terminate it last.
  // This ensures that the GC process is running whenever we terminate
  // any GC-managed process, which is necessary to prevent leaking.
  while (true) {
    // NOTE: We terminate by `UPID` rather than `ProcessBase` as the
    // process may terminate between the synchronized section below
    // and the calls to `process:terminate` and `process::wait`.
    // If the process has already terminated, further termination
    // is a noop.
    UPID pid;

    synchronized (processes_mutex) {
      ProcessBase* process = nullptr;

      foreachvalue (ProcessBase* candidate, processes) {
        if (candidate == gc) {
          continue;
        }

        process = candidate;
        pid = candidate->self();
        break;
      }

      if (process == nullptr) {
        break;
      }
    }

    // Terminate this process but do not inject the message,
    // i.e. allow it to finish its work first.
    process::terminate(pid, false);
    process::wait(pid);
  }

  // Terminate `gc`.
  process::terminate(gc, false);
  process::wait(gc);

  synchronized (processes_mutex) {
    delete gc;
    gc = nullptr;
  }

  // Send signal to all processing threads to stop running.
  joining_threads.store(true);
  runq.decomission();
  EventLoop::stop();

  // Join all threads.
  foreach (std::thread* thread, threads) {
    thread->join();
    delete thread;
  }
}


long ProcessManager::init_threads()
{
  // We create no fewer than 8 threads because some tests require
  // more worker threads than `sysconf(_SC_NPROCESSORS_ONLN)` on
  // computers with fewer cores.
  // e.g. https://issues.apache.org/jira/browse/MESOS-818
  //
  // TODO(xujyan): Use a smarter algorithm to allocate threads.
  // Allocating a static number of threads can cause starvation if
  // there are more waiting Processes than the number of worker
  // threads. On error assumes one core.
  long num_worker_threads =
    std::max(8L, os::cpus().isSome() ? os::cpus().get() : 1);

  // We allow the operator to set the number of libprocess worker
  // threads, using an environment variable. The motivation is that
  // for machines with a large number of cores, setting the number of
  // worker threads to sysconf(_SC_NPROCESSORS_ONLN) can be very high
  // and affect performance negatively. Furthermore, libprocess is
  // widely used in Mesos and there may be a large number of Mesos
  // processes (e.g., executors) on the same machine.
  //
  // See https://issues.apache.org/jira/browse/MESOS-4353.
  constexpr char env_var[] = "LIBPROCESS_NUM_WORKER_THREADS";
  Option<string> value = os::getenv(env_var);
  if (value.isSome()) {
    constexpr long maxval = 1024;
    Try<long> number = numify<long>(value.get().c_str());
    if (number.isSome() && number.get() > 0L && number.get() <= maxval) {
      VLOG(1) << "Overriding default number of worker threads "
              << num_worker_threads << ", using the value "
              << env_var << "=" << number.get() << " instead";
      num_worker_threads = number.get();
    } else {
      LOG(WARNING) << "Ignoring invalid value " << value.get()
                   << " for " << env_var
                   << ", using default value " << num_worker_threads
                   << ". Valid values are integers in the range 1 to "
                   << maxval;
    }
  }

  if (runq.capacity() < (size_t) num_worker_threads) {
    EXIT(EXIT_FAILURE) << "Number of worker threads can not exceed "
                       << runq.capacity() << " at this time";
  }

  threads.reserve(num_worker_threads + 1);

  // Create processing threads.
  for (long i = 0; i < num_worker_threads; i++) {
    // Retain the thread handles so that we can join when shutting down.
    threads.emplace_back(new std::thread(
        [this]() {
          running.fetch_add(1);
          do {
            ProcessBase* process = dequeue();
            if (process == nullptr) {
              if (joining_threads.load()) {
                break;
              }
            } else {
              resume(process);
            }
          } while (true);
          running.fetch_sub(1);

          // Threads are joining. Delete the thread local `_executor_`
          // pointer to prevent a memory leak.
          delete _executor_;
          _executor_ = nullptr;
        }));
  }

  // Create a thread for the event loop.
  threads.emplace_back(new std::thread(&EventLoop::run));

  return num_worker_threads;
}


ProcessReference ProcessManager::use(const UPID& pid)
{
  if (pid.reference.isSome()) {
    if (std::shared_ptr<ProcessBase*> reference = pid.reference->lock()) {
      return ProcessReference(std::move(reference));
    }
  }

  if (pid.address == __address__) {
    synchronized (processes_mutex) {
      Option<ProcessBase*> process = processes.get(pid.id);
      if (process.isSome()) {
        return ProcessReference(process.get()->reference);
      }
    }
  }

  return ProcessReference();
}


void ProcessManager::handle(
    const Socket& socket,
    Request* request)
{
  CHECK(request != nullptr);

  // Start by checking that the path starts with a '/'.
  if (request->url.path.find('/') != 0) {
    VLOG(1) << "Returning '400 Bad Request' for '" << request->url.path << "'";

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(socket);

    // Enqueue the response with the HttpProxy so that it respects the
    // order of requests to account for HTTP/1.1 pipelining.
    dispatch(
        proxy,
        &HttpProxy::enqueue,
        BadRequest("Request URL path must start with '/'"),
        *request);

    // Cleanup request.
    delete request;
    return;
  }

  // Check if this is a libprocess request (i.e., 'User-Agent:
  // libprocess/id@ip:port') and if so, parse as a message.
  if (libprocess(request)) {
    // It is guaranteed that the continuation would run before the next
    // request arrives. Also, it's fine to pass the `this` pointer to the
    // continuation as this would get executed synchronously (if still pending)
    // from `SocketManager::finalize()` due to it closing all active sockets
    // during libprocess finalization.
    parse(*request)
      .onAny([this, socket, request](const Future<MessageEvent*>& future) {
        // Get the HttpProxy pid for this socket.
        PID<HttpProxy> proxy = socket_manager->proxy(socket);

        if (!future.isReady()) {
          Response response = InternalServerError(
              future.isFailed() ? future.failure() : "discarded future");

          dispatch(proxy, &HttpProxy::enqueue, response, *request);

          VLOG(1) << "Returning '" << response.status << "' for '"
                  << request->url.path << "': " << response.body;

          delete request;
          return;
        }

        MessageEvent* event = CHECK_NOTNULL(future.get());

        // Verify that the UPID this peer is claiming is on the same IP
        // address the peer is sending from.
        if (libprocess_flags->require_peer_address_ip_match) {
          CHECK_SOME(request->client);

          // If the client address is not an IP address (e.g. coming
          // from a domain socket), we also reject the message.
          Try<Address> client_ip_address =
            network::convert<Address>(request->client.get());

          if (client_ip_address.isError() ||
              event->message.from.address.ip != client_ip_address->ip) {
            Response response = BadRequest(
                "UPID IP address validation failed: Message from " +
                stringify(event->message.from) + " was sent from IP " +
                stringify(request->client.get()));

            dispatch(proxy, &HttpProxy::enqueue, response, *request);

            VLOG(1) << "Returning '" << response.status << "'"
                    << " for '" << request->url.path << "'"
                    << ": " << response.body;

            delete request;
            delete event;
            return;
          }
        }

        // TODO(benh): Use the sender PID when delivering in order to
        // capture happens-before timing relationships for testing.
        bool accepted = deliver(event->message.to, event);

        // NOTE: prior to commit d5fe51c on April 11, 2014 we needed
        // to ignore sending responses in the event the receiver was a
        // version of libprocess that didn't properly ignore
        // responses. Now we always send a response.
        if (accepted) {
          VLOG(2) << "Delivered libprocess message to " << request->url.path;
          dispatch(proxy, &HttpProxy::enqueue, Accepted(), *request);
        } else {
          VLOG(1) << "Failed to deliver libprocess message to "
                  << request->url.path;
          dispatch(proxy, &HttpProxy::enqueue, NotFound(), *request);
        }

        delete request;
        return;
      });

    return;
  }

  // Treat this as an HTTP request.

  // Ignore requests with relative paths (i.e., contain "/..").
  if (request->url.path.find("/..") != string::npos) {
    VLOG(1) << "Returning '404 Not Found' for '" << request->url.path
            << "' (ignoring requests with relative paths)";

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(socket);

    // Enqueue the response with the HttpProxy so that it respects the
    // order of requests to account for HTTP/1.1 pipelining.
    dispatch(proxy, &HttpProxy::enqueue, NotFound(), *request);

    // Cleanup request.
    delete request;
    return;
  }

  // Split the path by '/'.
  vector<string> tokens = strings::tokenize(request->url.path, "/");

  // Try and determine a receiver, otherwise try and delegate.
  UPID receiver;

  if (tokens.size() == 0 && delegate.isSome()) {
    request->url.path = "/" + delegate.get();
    receiver = UPID(delegate.get(), __address__);
  } else if (tokens.size() > 0) {
    // Decode possible percent-encoded path.
    Try<string> decode = http::decode(tokens[0]);
    if (!decode.isError()) {
      receiver = UPID(decode.get(), __address__);
    } else {
      VLOG(1) << "Failed to decode URL path: " << decode.error();
    }
  }

  if (!use(receiver) && delegate.isSome()) {
    // Try and delegate the request.
    request->url.path = "/" + delegate.get() + request->url.path;
    receiver = UPID(delegate.get(), __address__);
  }

  synchronized (firewall_mutex) {
    // Don't use a const reference, since it cannot be guaranteed
    // that the rules don't keep an internal state.
    foreach (Owned<firewall::FirewallRule>& rule, firewallRules) {
      Option<Response> rejection = rule->apply(socket, *request);
      if (rejection.isSome()) {
        VLOG(1) << "Returning '"<< rejection.get().status << "' for '"
                << request->url.path << "' (firewall rule forbids request)";

        // TODO(arojas): Get rid of the duplicated code to return an
        // error.

        // Get the HttpProxy pid for this socket.
        PID<HttpProxy> proxy = socket_manager->proxy(socket);

        // Enqueue the response with the HttpProxy so that it respects
        // the order of requests to account for HTTP/1.1 pipelining.
        dispatch(
            proxy,
            &HttpProxy::enqueue,
            rejection.get(),
            *request);

        // Cleanup request.
        delete request;
        return;
      }
    }
  }

  if (use(receiver)) {
    // The promise is created here but its ownership is passed
    // into the HttpEvent created below.
    Promise<Response>* promise(new Promise<Response>());

    PID<HttpProxy> proxy = socket_manager->proxy(socket);

    // Enqueue the response with the HttpProxy so that it respects the
    // order of requests to account for HTTP/1.1 pipelining.
    dispatch(proxy, &HttpProxy::handle, promise->future(), *request);

    // TODO(benh): Use the sender PID in order to capture
    // happens-before timing relationships for testing.
    deliver(receiver, new HttpEvent(request, promise));

    return;
  }

  // This has no receiver, send error response.
  VLOG(1) << "Returning '404 Not Found' for '" << request->url.path << "'";

  // Get the HttpProxy pid for this socket.
  PID<HttpProxy> proxy = socket_manager->proxy(socket);

  // Enqueue the response with the HttpProxy so that it respects the
  // order of requests to account for HTTP/1.1 pipelining.
  dispatch(proxy, &HttpProxy::enqueue, NotFound(), *request);

  // Cleanup request.
  delete request;
}


bool ProcessManager::deliver(
    ProcessBase* receiver,
    Event* event,
    ProcessBase* sender)
{
  CHECK(event != nullptr);

  // If we are using a manual clock then update the current time of
  // the receiver using the sender if necessary to preserve the
  // happens-before relationship between the sender and receiver. Note
  // that the assumption is that the sender remains valid for at least
  // the duration of this routine (so that we can look up its current
  // time).
  if (Clock::paused()) {
    Clock::update(
        receiver, Clock::now(sender != nullptr ? sender : __process__));
  }

  receiver->enqueue(event);

  return true;
}


bool ProcessManager::deliver(
    const UPID& to,
    Event* event,
    ProcessBase* sender)
{
  CHECK(event != nullptr);

  if (ProcessReference receiver = use(to)) {
    return deliver(receiver, event, sender);
  }

  VLOG(2) << "Dropping event for process " << to;

  delete event;
  return false;
}


UPID ProcessManager::spawn(ProcessBase* process, bool manage)
{
  CHECK(process != nullptr);

  // If the `ProcessManager` is cleaning itself up, no further processes
  // may be spawned.
  if (finalizing.load()) {
    LOG(WARNING)
      << "Attempted to spawn a process (" << process->self()
      << ") after finalizing libprocess!";

    if (manage) {
      delete process;
    }

    return UPID();
  }

  synchronized (processes_mutex) {
    if (processes.count(process->pid.id) > 0) {
      VLOG(1) << "Attempting to spawn already spawned process " << process->pid;
      return UPID();
    } else {
      processes[process->pid.id] = process;

      // NOTE: we set process reference on it's `UPID` _after_ we've
      // spawned so that we make sure that we'll take the
      // `ProcessManager::use()` code path in the event that we aren't
      // able to spawn the process. This is important in circumstances
      // where there are multiple processes with the same ID because
      // the semantics that people have come to expect from libprocess
      // is that a `UPID` should "resolve" to the already spawned
      // process rather than a process that has the same name but
      // hasn't yet been spawned.
      process->pid.reference = process->reference;
    }
  }

  // Use the garbage collector if requested.
  if (manage) {
    dispatch(gc->self(), &GarbageCollector::manage<ProcessBase>, process);
  }

  // We save the PID before enqueueing the process to avoid the race
  // condition that occurs when a user has a very short process and
  // the process gets run and cleaned up before we return from enqueue
  // (e.g., when 'manage' is set to true).
  UPID pid = process->self();

  // Add process to the run queue (so 'initialize' will get invoked).
  enqueue(process);

  VLOG(2) << "Spawned process " << pid;

  return pid;
}


void ProcessManager::resume(ProcessBase* process)
{
  __process__ = process;

  VLOG(2) << "Resuming " << process->pid << " at " << Clock::now();

  bool terminate = false;
  bool blocked = false;

  ProcessBase::State state = process->state.load();

  CHECK(state == ProcessBase::State::BOTTOM ||
        state == ProcessBase::State::READY);

  if (state == ProcessBase::State::BOTTOM) {
    try { process->initialize(); }
    catch (...) { terminate = true; }
  }

  while (!terminate && !blocked) {
    Event* event = nullptr;

    // NOTE: the event queue requires only a _single_ consumer at a
    // time ... this is where we act as that single consumer (and down
    // in `ProcessManager::cleanup` which we call from here).

    if (!process->events->consumer.empty()) {
      event = process->events->consumer.dequeue();
    } else {
      state = ProcessBase::State::BLOCKED;
      process->state.store(state);
      blocked = true;

      // Now check that we didn't miss any events that got added
      // before we set ourselves to BLOCKED since we won't have been
      // added to the run queue in those circumstances so we need to
      // serve those events!
      if (!process->events->consumer.empty()) {
        // Make sure the state is in READY! Either we need to
        // explicitly do this because `ProcessBase::enqueue` saw us as
        // READY (or BOTTOM) and didn't change the state or we're
        // racing with `ProcessBase::enqueue` because they saw us at
        // BLOCKED and are trying to change the state. If they change
        // the state then they'll also enqueue this process, which
        // means we need to bail because another thread might resume
        // (and the reason we'll bail is because `blocked` is true)!
        if (process->state.compare_exchange_strong(
                state,
                ProcessBase::State::READY)) {
          blocked = false;
          continue;
        }
      }
    }

    if (!blocked) {
      CHECK_NOTNULL(event);

      // Before serving this event check if we've triggered a
      // terminate and if so purge all events until we get to the
      // terminate event.
      terminate = process->termination.load();
      if (terminate) {
        // Now purge all events until the terminate event.
        while (!event->is<TerminateEvent>()) {
          delete event;
          event = process->events->consumer.dequeue();
          CHECK_NOTNULL(event);
        }
      }

      // Determine if we should filter this event.
      //
      // NOTE: we use double-checked locking here to avoid
      // head-of-line blocking that occurs when the first thread
      // attempts to filter an event.
      //
      // TODO(benh): While not critical for production systems because
      // the filter should not be set in production systems, we could
      // use a reader/writer lock here in addition to double-checked
      // locking.
      //
      // TODO(benh): Consider optimizing this further to not be
      // sequentially consistent. For more details see:
      // http://preshing.com/20130930/double-checked-locking-is-fixed-in-cpp11.
      if (filter.load() != nullptr) {
        synchronized (filter_mutex) {
          Filter* f = filter.load();
          if (f != nullptr && f->filter(event)) {
            delete event;
            continue; // Try and execute the next event.
          }
        }
      }

      // Determine if we should terminate.
      terminate = event->is<TerminateEvent>();

      // Now service the event.
      try {
        process->serve(*event);
      } catch (const std::exception& e) {
        std::cerr << "libprocess: " << process->pid
                  << " terminating due to "
                  << e.what() << std::endl;
        terminate = true;
      } catch (...) {
        std::cerr << "libprocess: " << process->pid
                  << " terminating due to unknown exception" << std::endl;
        terminate = true;
      }

      delete event;

      if (terminate) {
        cleanup(process);
      }
    }
  }

  // TODO(benh): If `terminate` was set to true when we initialized
  // then we'll never actually cleanup! This bug has been here a long
  // time!!!

  __process__ = nullptr;
}


void ProcessManager::cleanup(ProcessBase* process)
{
  VLOG(2) << "Cleaning up " << process->pid;

  // First, set the terminating state so no more events will get
  // enqueued and then decomission the event queue which will also
  // delete all the pending events. We want to delete the events
  // before we hold `processes_mutex` because deleting an event could
  // cause code outside libprocess to get executed which might cause a
  // deadlock with `processes_mutex`. Also, deleting the events now
  // rather than later has the nice property of making sure that any
  // _new_ events that might have gotten enqueued _BACK_ onto this
  // process due to the deleting of the pending events will get
  // dropped since this process is now TERMINATING, which eliminates
  // the potential of these new events from getting enqueued onto a
  // _new_ process that gets spawned with the same PID.
  process->state.store(ProcessBase::State::TERMINATING);

  process->events->consumer.decomission();

  // Remove help strings for all installed routes for this process.
  dispatch(help, &Help::remove, process->pid.id);

  // Possible gate non-libprocess threads are waiting at.
  std::shared_ptr<Gate> gate = process->gate;

  // Remove process.
  synchronized (processes_mutex) {
    // Reset the reference so that we don't keep giving out references
    // in `ProcessManager::use`.
    //
    // NOTE: this must be done from within the `processes_mutex` since
    // that is where we read it and this is considered a write.
    process->reference.reset();

    // Wait for all process references to get cleaned up.
    CHECK_SOME(process->pid.reference);
    while (!process->pid.reference->expired()) {
#if defined(__i386__) || defined(__x86_64__)
      asm ("pause");
#endif
    }

    processes.erase(process->pid.id);

    // Note that we don't remove the process from the clock during
    // cleanup, but rather the clock is reset for a process when it is
    // created (see ProcessBase::ProcessBase). We do this so that
    // SocketManager::exited can access the current time of the
    // process to "order" exited events. TODO(benh): It might make
    // sense to consider storing the time of the process as a field of
    // the class instead.

    // Now we tell the socket manager about this process exiting so
    // that it can create exited events for linked processes. We
    // _must_ do this while synchronized on processes because
    // otherwise another process could attempt to link this process
    // and SocketManager::link would see that the processes doesn't
    // exist when it attempts to get a ProcessReference (since we
    // removed the process above) thus causing an exited event, which
    // could cause the process to get deleted (e.g., the garbage
    // collector might link _after_ the process has already been
    // removed from processes thus getting an exited event but we
    // don't want that exited event to fire and actually delete the
    // process until after we have used the process in
    // SocketManager::exited).
    socket_manager->exited(process);

    // ***************************************************************
    // At this point we can no longer dereference the process since it
    // might already be deallocated (e.g., by the garbage collector).
    // ***************************************************************

    // Note that we need to open the gate while synchronized on
    // processes because otherwise we might _open_ the gate before
    // another thread _approaches_ the gate causing that thread to
    // wait on _arrival_ to the gate forever (see
    // ProcessManager::wait).
    CHECK(gate);
    gate->open();
  }
}


void ProcessManager::link(
    ProcessBase* process,
    const UPID& to,
    const ProcessBase::RemoteConnection remote)
{
  // Check if the pid is local.
  if (to.address != __address__) {
    socket_manager->link(process, to, remote);
  } else {
    // Since the pid is local we want to get a reference to its
    // underlying process so that while we are invoking the link
    // manager we don't miss sending a possible ExitedEvent.
    if (ProcessReference _ = use(to)) {
      socket_manager->link(process, to, remote);
    } else {
      // Since the pid isn't valid its process must have already died
      // (or hasn't been spawned yet) so send a process exit message.
      process->enqueue(new ExitedEvent(to));
    }
  }
}


void ProcessManager::terminate(
    const UPID& pid,
    bool inject,
    ProcessBase* sender)
{
  if (ProcessReference process = use(pid)) {
    if (Clock::paused()) {
      Clock::update(
          process, Clock::now(sender != nullptr ? sender : __process__));
    }

    if (sender != nullptr) {
      process->enqueue(new TerminateEvent(sender->self(), inject));
    } else {
      process->enqueue(new TerminateEvent(UPID(), inject));
    }
  }
}


bool ProcessManager::wait(const UPID& pid)
{
  std::shared_ptr<Gate> gate;

  ProcessBase* process = nullptr; // Set to non-null if we donate thread.

  if (ProcessReference reference = use(pid)) {
    // Save the process assuming we can donate to it.
    process = reference;

    gate = process->gate;

    // Check if it is runnable in order to donate this thread.
    switch (process->state.load()) {
      case ProcessBase::State::BOTTOM:
      case ProcessBase::State::READY:
        // Assume that we'll be able to successfully extract the
        // process from the run queue and optimistically increment
        // `running` so that `Clock::settle` properly waits. In the
        // event that we aren't able to extract the process from the
        // run queue then we'll decrement `running`. Note that we
        // can't assume that `running` is already non-zero because any
        // thread may call `wait`, and thus we can't assume that we're
        // calling it from a process that is already running.
        running.fetch_add(1);

        // Try and extract the process from the run queue. This may
        // fail because another thread might resume the process first
        // or the run queue might not support arbitrary extraction.
        if (!runq.extract(process)) {
          running.fetch_sub(1);
          process = nullptr;
        }
        break;
      case ProcessBase::State::BLOCKED:
      case ProcessBase::State::TERMINATING:
        process = nullptr;
        break;
    }
  }

  if (process != nullptr) {
    VLOG(2) << "Donating thread to " << process->pid << " while waiting";
    ProcessBase* donator = __process__;
    resume(process);
    running.fetch_sub(1);
    __process__ = donator;
  }

  // NOTE: `process` is possibly deleted at this point and we must not
  // use it!

  // TODO(benh): Donating only once may not be sufficient, so we might
  // still deadlock here ... perhaps warn if that's the case?
  //
  // In fact, we might want to support the ability to donate a thread
  // to any process for a limited number of messages while we wait
  // (i.e., donate for a message, check and see if our gate is open,
  // if not, keep donating).

  // Now wait at the gate until it opens.
  if (gate) {
    gate->wait();
    return true;
  }

  return false;
}


void ProcessManager::installFirewall(
    vector<Owned<firewall::FirewallRule>>&& rules)
{
  synchronized (firewall_mutex) {
    firewallRules = std::move(rules);
  }
}


string ProcessManager::absolutePath(const string& path)
{
  // Return directly when delegate is empty.
  if (delegate.isNone()) {
    return path;
  }

  vector<string> tokens = strings::tokenize(path, "/");

  // Return delegate when path is root.
  if (tokens.size() == 0) {
    return "/" + delegate.get();
  }

  Try<string> decode = http::decode(tokens[0]);

  // Return path when decode failed
  if (decode.isError()) {
    VLOG(1) << "Failed to decode URL path: " << decode.error();
    return path;
  }

  if (processes.find(decode.get()) != processes.end()) {
    // Return path when the first token is a process id.
    return path;
  } else {
    return "/" + delegate.get() + path;
  }
}


void ProcessManager::enqueue(ProcessBase* process)
{
  CHECK(process != nullptr);

  // If libprocess is shutting down and the processing threads
  // are currently joining, then do not enqueue the process.
  if (joining_threads.load()) {
    VLOG(1) << "Libprocess shutting down, cannot enqueue process: "
            << process->pid.id;

    return;
  }

  // TODO(benh): Check and see if this process has its own thread. If
  // it does, push it on that threads runq, and wake up that thread if
  // it's not running. Otherwise, check and see which thread this
  // process was last running on, and put it on that threads runq.

  runq.enqueue(process);
}


ProcessBase* ProcessManager::dequeue()
{
  // TODO(benh): Remove a process from this thread's runq. If there
  // are no processes to run, and this is not a dedicated thread, then
  // steal one from another threads runq.

  running.fetch_sub(1);

  runq.wait();

  // Need to increment `running` before we dequeue from `runq` so that
  // `Clock::settle` properly waits.
  running.fetch_add(1);

  ////////////////////////////////////////////////////////////
  // NOTE: contract with the run queue is that we'll always //
  // call `wait` _BEFORE_ we call `dequeue`.                //
  ////////////////////////////////////////////////////////////
  return runq.dequeue();
}


// NOTE: it's possible that a thread not controlled by libprocess is
// trying to enqueue a process (e.g., due to `spawn` or because it's
// doing a `dispatch` or `send`) and thus we'll settle when in fact we
// should not have. There is nothing easy we can do to prevent this
// and it hasn't been a problem historically in the usage we've seen
// in the Mesos project.
void ProcessManager::settle()
{
  bool done = true;
  do {
    done = true; // Assume to start that we are settled.

    // See comments below as to how `epoch` helps us mitigate races
    // with `running` and `runq`.
    long old = runq.epoch.load();

    if (running.load() > 0) {
      done = false;
      continue;
    }

    // Race #1: it's possible that a thread starts running here
    // because the semaphore had been signaled but nobody has woken
    // up yet.

    if (!runq.empty()) {
      done = false;
      continue;
    }

    // Race #2: it's possible that `runq` will get added to at this
    // point given some threads might be running due to 'Race #1'.

    if (running.load() > 0) {
      done = false;
      continue;
    }

    // If at this point _no_ threads are running then it must be the
    // case that either nothing has been added to `runq` (and thus
    // nothing really is running or will be about to run) OR
    // `runq.epoch` must have been incremented (because the thread
    // that enqueued something into `runq.epoch` now isn't running so
    // it must have incremented `runq.epoch` before it decremented
    // `running`).
    //
    // Note that we check `runq.epoch` _after_ we check the clock
    // because it's possible that the clock will also add to the
    // `runq` but in so doing it will also increment `runq.epoch`
    // which we'll guarantee that we don't settle (and
    // `Clock::settled()` takes care to atomically ensure that
    // `runq.epoch` is incremented before it returns).

    if (!Clock::settled()) {
      done = false;
      continue;
    }

    if (old != runq.epoch.load()) {
      done = false;
      continue;
    }
  } while (!done);
}


Future<Response> ProcessManager::__processes__(const Request&)
{
  synchronized (processes_mutex) {
    return collect(lambda::map(
        [](ProcessBase* process) {
          // TODO(benh): Try and "inject" this dispatch or create a
          // high-priority set of events (i.e., mailbox).
          return dispatch(
              process->self(),
              [process]() -> JSON::Object {
                return *process;
              });
        },
        process_manager->processes.values()))
      .then([](const std::list<JSON::Object>& objects) -> Response {
        JSON::Array array;
        foreach (const JSON::Object& object, objects) {
          array.values.push_back(object);
        }
        return OK(array);
      });
  }
}


ProcessBase::ProcessBase(const string& id)
  : events(new EventQueue()),
    reference(std::make_shared<ProcessBase*>(this)),
    gate(std::make_shared<Gate>())
{
  process::initialize();

  pid.id = id != "" ? id : ID::generate();
  pid.address = __address__;
  pid.addresses.v6 = __address6__;

  // If using a manual clock, try and set current time of process
  // using happens before relationship between creator (__process__)
  // and createe (this)!
  if (Clock::paused()) {
    Clock::update(this, Clock::now(__process__), Clock::FORCE);
  }
}


ProcessBase::~ProcessBase() {}


template <>
size_t ProcessBase::eventCount<MessageEvent>()
{
  CHECK_EQ(this, __process__);
  return events->consumer.count<MessageEvent>();
}


template <>
size_t ProcessBase::eventCount<DispatchEvent>()
{
  CHECK_EQ(this, __process__);
  return events->consumer.count<DispatchEvent>();
}


template <>
size_t ProcessBase::eventCount<HttpEvent>()
{
  CHECK_EQ(this, __process__);
  return events->consumer.count<HttpEvent>();
}


template <>
size_t ProcessBase::eventCount<ExitedEvent>()
{
  CHECK_EQ(this, __process__);
  return events->consumer.count<ExitedEvent>();
}


template <>
size_t ProcessBase::eventCount<TerminateEvent>()
{
  CHECK_EQ(this, __process__);
  return events->consumer.count<TerminateEvent>();
}


void ProcessBase::enqueue(Event* event)
{
  CHECK_NOTNULL(event);

  State old = state.load();

  // Need to check if this is a terminate event _BEFORE_ we enqueue
  // because it's possible that it'll get deleted after we enqueue it
  // and before we use it again!
  bool terminate =
    event->is<TerminateEvent>() &&
    event->as<TerminateEvent>().inject;

  switch (old) {
    case State::BOTTOM:
    case State::READY:
    case State::BLOCKED:
      events->producer.enqueue(event);
      break;
    case State::TERMINATING:
      delete event;
      return;
  }

  // We need to store terminate _AFTER_ we enqueue the event because
  // the code in `ProcessMNager::resume` assumes that if it sees
  // `termination` as true then there must be at least one event in
  // the queue.
  if (terminate) {
    termination.store(true);
  }

  // If we're BLOCKED then we need to try and enqueue us into the run
  // queue. It's possible that in the time we enqueued the event and
  // are attempting to enqueue us in the run queue another thread has
  // already served the event! Worse case scenario we'll end up
  // enqueuing us in the run queue only to find out in
  // `ProcessManager::resume` that our event queue is empty!
  if ((old = state.load()) == State::BLOCKED) {
    if (state.compare_exchange_strong(old, State::READY)) {
      // NOTE: we only enqueue if _we_ successfully did the exchange
      // since another thread executing this code or a thread in
      // `ProcessBase::resume` might have already done the exchange to
      // READY.
      process_manager->enqueue(this);
    }
  }
}


void ProcessBase::send(
    const UPID& to,
    const string& name,
    const char* data,
    size_t length)
{
  if (!to) {
    return;
  }

  // Encode and transport outgoing message.
  transport(pid, to, name, data, length, this);
}


void ProcessBase::send(
    const UPID& to,
    string&& name,
    const char* data,
    size_t length)
{
  if (!to) {
    return;
  }

  // Encode and transport outgoing message.
  transport(pid, to, std::move(name), data, length, this);
}


void ProcessBase::visit(const MessageEvent& event)
{
  if (handlers.message.count(event.message.name) > 0) {
    handlers.message[event.message.name](
        event.message.from,
        event.message.body);
  } else if (delegates.count(event.message.name) > 0) {
    VLOG(1) << "Delegating message '" << event.message.name
            << "' to " << delegates[event.message.name];
    Message message(event.message);
    message.to = delegates[event.message.name];
    transport(std::move(message), this);
  }
}


void ProcessBase::visit(const DispatchEvent& event)
{
  (*event.f)(this);
}


void ProcessBase::visit(const HttpEvent& event)
{
  VLOG(1) << "Handling HTTP event for process '" << pid.id << "'"
          << " with path: '" << event.request->url.path << "'";

  // Lazily initialize the Sequence needed for ordering requests
  // across authentication and authorization.
  if (handlers.httpSequence.get() == nullptr) {
    handlers.httpSequence.reset(new Sequence("__auth_handlers__"));
  }

  const string& path = event.request->url.path;

  CHECK(path.find('/') == 0); // See ProcessManager::handle.

  // Split the path by '/'.
  vector<string> tokens = strings::tokenize(path, "/");
  CHECK(!tokens.empty());

  const string id = http::decode(tokens[0]).get();
  CHECK_EQ(pid.id, id);

  // First look to see if there is an HTTP handler that can handle the
  // longest prefix of this path.

  // Remove the `id` prefix from the path.
  string name = strings::remove(
      event.request->url.path, "/" + tokens[0], strings::PREFIX);
  name = strings::trim(name, strings::PREFIX, "/");

  // Look for an endpoint handler for this path. We begin with the full path,
  // but if no handler is found and the path is nested, we shorten it and look
  // again. For example: if the request is for '/a/b/c' and no handler is found,
  // we will then check for '/a/b', and finally for '/a'.
  while (Path(name).dirname() != name) {
    if (handlers.http.count(name) == 0) {
      name = Path(name).dirname();
      continue;
    }

    const HttpEndpoint& endpoint = handlers.http[name];

    Owned<Request> request(new Request(*event.request));
    Future<Response> response;

    if (!endpoint.options.requestStreaming) {
      // Consume the request body on behalf of the endpoint.
      response = convert(std::move(request))
        .then(defer(self(), [this, endpoint, name](
            const Owned<Request>& request) {
          return _visit(endpoint, name, request);
        }));
    } else {
      response = _visit(endpoint, name, request);
    }

    response
      .onAny([path](const Future<Response>& response) {
        if (!response.isReady()) {
          VLOG(1) << "Failed to process request for '" << path << "': "
                  << (response.isFailed() ? response.failure() : "discarded");
        }
      });

    event.response->associate(response);
    return;
  }

  // Ensure the body is consumed so that no backpressure is applied
  // to the socket (ignore the content since we do not care about it).
  //
  // TODO(anand): Is this an error?
  CHECK_SOME(event.request->reader);
  event.request->reader->readAll();

  // If no HTTP handler is found look in assets.
  name = tokens.size() > 1 ? tokens[1] : "";

  if (assets.count(name) > 0) {
    OK response;
    response.type = Response::PATH;
    response.path = assets[name].path;

    // Construct the final path by appending remaining tokens.
    for (size_t i = 2; i < tokens.size(); i++) {
      response.path += "/" + tokens[i];
    }

    // Try and determine the Content-Type from an extension.
    Option<string> extension = Path(response.path).extension();

    if (extension.isSome() && assets[name].types.count(extension.get()) > 0) {
      response.headers["Content-Type"] = assets[name].types[extension.get()];
    }

    // TODO(benh): Use "text/plain" for assets that don't have an
    // extension or we don't have a mapping for? It might be better to
    // just let the browser guess (or do its own default).

    event.response->associate(response);

    return;
  }

  VLOG(1) << "Returning '404 Not Found' for"
          << " '" << event.request->url.path << "'";

  event.response->associate(NotFound());
}


Future<Response> ProcessBase::_visit(
    const HttpEndpoint& endpoint,
    const string& name,
    const Owned<Request>& request)
{
  Future<Option<AuthenticationResult>> authentication = None();

  if (endpoint.realm.isSome()) {
    authentication = authenticator_manager->authenticate(
        *request, endpoint.realm.get());
  }

  // Sequence the authentication future to ensure the handlers
  // are invoked in the same order that requests arrive.
  authentication = handlers.httpSequence->add<Option<AuthenticationResult>>(
      [authentication]() { return authentication; });

  return authentication
    .then(defer(self(), [this, endpoint, request, name](
        const Option<AuthenticationResult>& authentication)
          -> Future<Response> {
      Option<Principal> principal = None();

      // If authentication failed, we do not continue with authorization.
      if (authentication.isSome()) {
        if (authentication->unauthorized.isSome()) {
          // Request was not authenticated, challenged issued.
          return authentication->unauthorized.get();
        } else if (authentication->forbidden.isSome()) {
          // Request was not authenticated, no challenge issued.
          return authentication->forbidden.get();
        }

        CHECK_SOME(authentication->principal);
        principal = authentication->principal;
      }

      // The result of a call to an authorization callback.
      Future<bool> authorization;

      // Look for an authorization callback installed for this endpoint path.
      // If none is found, use a trivial one.
      const string callback_path = path::join("/" + pid.id, name);
      if (authorization_callbacks != nullptr &&
          authorization_callbacks->count(callback_path) > 0) {
        authorization = authorization_callbacks->at(callback_path)(
            *request, principal);

        // Sequence the authorization future to ensure the handlers
        // are invoked in the same order that requests arrive.
        authorization = handlers.httpSequence->add<bool>(
            [authorization]() { return authorization; });
      } else {
        authorization = handlers.httpSequence->add<bool>(
            []() { return true; });
      }

      // Install a callback on the authorization result.
      return authorization
        .then(defer(self(), [endpoint, request, principal](
            bool authorization) -> Future<Response> {
          if (authorization) {
            // Authorization succeeded, so forward request to the handler.
            if (endpoint.realm.isNone()) {
              return endpoint.handler.get()(*request);
            }

            return endpoint.authenticatedHandler.get()(*request, principal);
          }

          // Authorization failed, so return a `Forbidden` response.
          return Forbidden();
        }
      ));
    }));
}


void ProcessBase::visit(const ExitedEvent& event)
{
  exited(event.pid);
}


void ProcessBase::visit(const TerminateEvent& event)
{
  finalize();
}


UPID ProcessBase::link(const UPID& to, const RemoteConnection remote)
{
  if (!to) {
    return to;
  }

  process_manager->link(this, to, remote);

  return to;
}


void ProcessBase::route(
    const string& name,
    const Option<string>& help_,
    const HttpRequestHandler& handler,
    const RouteOptions& options)
{
  // Routes must start with '/'.
  CHECK(name.find('/') == 0);

  HttpEndpoint endpoint;
  endpoint.handler = handler;
  endpoint.options = options;

  handlers.http[name.substr(1)] = endpoint;

  dispatch(help, &Help::add, pid.id, name, help_);
}


void ProcessBase::route(
    const string& name,
    const string& realm,
    const Option<string>& help_,
    const AuthenticatedHttpRequestHandler& handler,
    const RouteOptions& options)
{
  // Routes must start with '/'.
  CHECK(name.find('/') == 0);

  HttpEndpoint endpoint;
  endpoint.realm = realm;
  endpoint.authenticatedHandler = handler;
  endpoint.options = options;

  handlers.http[name.substr(1)] = endpoint;

  dispatch(help, &Help::add, pid.id, name, help_);
}


ProcessBase:: operator JSON::Object()
{
  CHECK_EQ(this, __process__);

  JSON::Object object;
  object.values["id"] = (const string&) pid.id;
  object.values["events"] = JSON::Array(events->consumer);
  return object;
}


void UPID::resolve()
{
  if (ProcessReference process = process_manager->use(*this)) {
    reference = process.reference;
  }
  // Otherwise keep it `None` to force look ups in the future!
}


UPID spawn(ProcessBase* process, bool manage)
{
  process::initialize();

  if (process != nullptr) {
    // If using a manual clock, try and set current time of process
    // using happens before relationship between spawner (__process__)
    // and spawnee (process)!
    if (Clock::paused()) {
      Clock::update(process, Clock::now(__process__));
    }

    return process_manager->spawn(process, manage);
  } else {
    return UPID();
  }
}


void terminate(const UPID& pid, bool inject)
{
  process_manager->terminate(pid, inject, __process__);
}


class WaitWaiter : public Process<WaitWaiter>
{
public:
  WaitWaiter(const UPID& _pid, const Duration& _duration, bool* _waited)
    : ProcessBase(ID::generate("__waiter__")),
      pid(_pid),
      duration(_duration),
      waited(_waited) {}

  virtual void initialize()
  {
    VLOG(3) << "Running waiter process for " << pid;
    link(pid);
    delay(duration, self(), &WaitWaiter::timeout);
  }

private:
  virtual void exited(const UPID&)
  {
    VLOG(3) << "Waiter process waited for " << pid;
    *waited = true;
    terminate(self());
  }

  void timeout()
  {
    VLOG(3) << "Waiter process timed out waiting for " << pid;
    *waited = false;
    terminate(self());
  }

private:
  const UPID pid;
  const Duration duration;
  bool* const waited;
};


bool wait(const UPID& pid, const Duration& duration)
{
  process::initialize();

  if (!pid) {
    return false;
  }

  // This could result in a deadlock if some code decides to wait on a
  // process that has invoked that code!
  if (__process__ != nullptr && __process__->self() == pid) {
    std::cerr << "\n**** DEADLOCK DETECTED! ****\nYou are waiting on process "
              << pid << " that it is currently executing." << std::endl;
  }

  if (duration == Seconds(-1)) {
    return process_manager->wait(pid);
  }

  bool waited = false;

  WaitWaiter waiter(pid, duration, &waited);
  spawn(waiter);
  wait(waiter);

  return waited;
}


void filter(Filter* filter)
{
  process::initialize();

  process_manager->install(filter);
}


void post(const UPID& to, const string& name, const char* data, size_t length)
{
  process::initialize();

  if (!to) {
    return;
  }

  // Transport outgoing message.
  transport(UPID(), to, name, data, length);
}


void post(const UPID& from,
          const UPID& to,
          const string& name,
          const char* data,
          size_t length)
{
  process::initialize();

  if (!to) {
    return;
  }

  // Transport outgoing message.
  transport(from, to, name, data, length);
}


namespace inject {

bool exited(const UPID& from, const UPID& to)
{
  process::initialize();

  ExitedEvent* event = new ExitedEvent(from);
  return process_manager->deliver(to, event, __process__);
}

} // namespace inject {


namespace internal {

void dispatch(
    const UPID& pid,
    const std::shared_ptr<lambda::function<void(ProcessBase*)>>& f,
    const Option<const std::type_info*>& functionType)
{
  process::initialize();

  DispatchEvent* event = new DispatchEvent(pid, f, functionType);
  process_manager->deliver(pid, event, __process__);
}

} // namespace internal {
} // namespace process {
