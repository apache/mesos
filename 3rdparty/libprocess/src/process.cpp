#include <errno.h>
#include <limits.h>
#include <libgen.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <glog/logging.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>

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
#include <utility>
#include <vector>

#include <process/address.hpp>
#include <process/check.hpp>
#include <process/clock.hpp>
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
#include <process/process.hpp>
#include <process/profiler.hpp>
#include <process/socket.hpp>
#include <process/statistics.hpp>
#include <process/system.hpp>
#include <process/time.hpp>
#include <process/timer.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/synchronized.hpp>
#include <stout/thread.hpp>
#include <stout/unreachable.hpp>

#include "config.hpp"
#include "decoder.hpp"
#include "encoder.hpp"
#include "event_loop.hpp"
#include "gate.hpp"
#include "process_reference.hpp"

using namespace process::firewall;
using namespace process::metrics::internal;

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

using process::network::Address;
using process::network::Socket;

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
// the underyling file descriptor from getting closed while there
// might still be outstanding responses even though the client might
// have closed the connection (see more discussion in
// SocketManger::close and SocketManager::proxy).
class HttpProxy : public Process<HttpProxy>
{
public:
  explicit HttpProxy(const Socket& _socket);
  virtual ~HttpProxy();

  // Enqueues the response to be sent once all previously enqueued
  // responses have been processed (e.g., waited for and sent).
  void enqueue(const Response& response, const Request& request);

  // Enqueues a future to a response that will get waited on (up to
  // some timeout) and then sent once all previously enqueued
  // responses have been processed (e.g., waited for and sent).
  void handle(Future<Response>* future, const Request& request);

private:
  // Starts "waiting" on the next available future response.
  void next();

  // Invoked once a future response has been satisfied.
  void waited(const Future<Response>& future);

  // Demuxes and handles a response.
  bool process(const Future<Response>& future, const Request& request);

  // Handles stream based responses.
  void stream(const Request& request, const Future<string>& chunk);

  Socket socket; // Wrap the socket to keep it from getting closed.

  // Describes a queue "item" that wraps the future to the response
  // and the original request.
  // The original request contains needed information such as what encodings
  // are acceptable and whether to persist the connection.
  struct Item
  {
    Item(const Request& _request, Future<Response>* _future)
      : request(_request), future(_future) {}

    ~Item()
    {
      delete future;
    }

    const Request request; // Make a copy.
    Future<Response>* future;
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
  {
    process = new RouteProcess(name, help, handler);
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

  RouteProcess* process;
};


class SocketManager
{
public:
  SocketManager();
  ~SocketManager();

  void accepted(const Socket& socket);

  void link(ProcessBase* process, const UPID& to);

  PID<HttpProxy> proxy(const Socket& socket);

  void send(Encoder* encoder, bool persist);
  void send(const Response& response,
            const Request& request,
            const Socket& socket);
  void send(Message* message);

  Encoder* next(int s);

  void close(int s);

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

  // Collection of all actice sockets.
  map<int, Socket*> sockets;

  // Collection of sockets that should be disposed when they are
  // finished being used (e.g., when there is no more data to send on
  // them).
  set<int> dispose;

  // Map from socket to socket address (ip, port).
  map<int, Address> addresses;

  // Maps from socket address (ip, port) to temporary sockets (i.e.,
  // they will get closed once there is no more data to send on them).
  map<Address, int> temps;

  // Maps from socket address (ip, port) to persistent sockets (i.e., they will
  // remain open even if there is no more data to send on them).  We
  // distinguish these from the 'temps' collection so we can tell when
  // a persistant socket has been lost (and thus generate
  // ExitedEvents).
  map<Address, int> persists;

  // Map from socket to outgoing queue.
  map<int, queue<Encoder*>> outgoing;

  // HTTP proxies.
  map<int, HttpProxy*> proxies;

  // Protects instance variables.
  std::recursive_mutex mutex;
};


class ProcessManager
{
public:
  explicit ProcessManager(const string& delegate);
  ~ProcessManager();

  ProcessReference use(const UPID& pid);

  bool handle(
      const Socket& socket,
      Request* request);

  bool deliver(
      ProcessBase* receiver,
      Event* event,
      ProcessBase* sender = NULL);

  bool deliver(
      const UPID& to,
      Event* event,
      ProcessBase* sender = NULL);

  UPID spawn(ProcessBase* process, bool manage);
  void resume(ProcessBase* process);
  void cleanup(ProcessBase* process);
  void link(ProcessBase* process, const UPID& to);
  void terminate(const UPID& pid, bool inject, ProcessBase* sender = NULL);
  bool wait(const UPID& pid);

  void installFirewall(std::vector<Owned<FirewallRule>>&& rules);

  void enqueue(ProcessBase* process);
  ProcessBase* dequeue();

  void settle();

  // The /__processes__ route.
  Future<Response> __processes__(const Request&);

private:
  // Delegate process name to receive root HTTP requests.
  const string delegate;

  // Map of all local spawned and running processes.
  map<string, ProcessBase*> processes;
  std::recursive_mutex processes_mutex;

  // Gates for waiting threads (protected by processes_mutex).
  map<ProcessBase*, Gate*> gates;

  // Queue of runnable processes (implemented using list).
  list<ProcessBase*> runq;
  std::recursive_mutex runq_mutex;

  // Number of running processes, to support Clock::settle operation.
  int running;

  // List of rules applied to all incoming HTTP connections.
  std::vector<Owned<FirewallRule>> firewallRules;
  std::recursive_mutex firewall_mutex;
};


// Unique id that can be assigned to each process.
static uint32_t __id__ = 0;

// Server socket listen backlog.
static const int LISTEN_BACKLOG = 500000;

// Local server socket.
static Socket* __s__ = NULL;

// Local socket address.
static Address __address__;

// Active SocketManager (eventually will probably be thread-local).
static SocketManager* socket_manager = NULL;

// Active ProcessManager (eventually will probably be thread-local).
static ProcessManager* process_manager = NULL;

// Scheduling gate that threads wait at when there is nothing to run.
static Gate* gate = new Gate();

// Filter. Synchronized support for using the filterer needs to be
// recursive in case a filterer wants to do anything fancy (which is
// possible and likely given that filters will get used for testing).
static Filter* filterer = NULL;
static std::recursive_mutex* filterer_mutex = new std::recursive_mutex();

// Global garbage collector.
PID<GarbageCollector> gc;

// Global help.
PID<Help> help;

// Per thread process pointer.
ThreadLocal<ProcessBase>* _process_ = new ThreadLocal<ProcessBase>();

// Per thread executor pointer.
ThreadLocal<Executor>* _executor_ = new ThreadLocal<Executor>();

// TODO(dhamon): Reintroduce this when it is plumbed through to Statistics.
// const Duration LIBPROCESS_STATISTICS_WINDOW = Days(1);


// NOTE: Clock::* implementations are in clock.cpp except for
// Clock::settle which currently has a dependency on
// 'process_manager'.
void Clock::settle()
{
  process_manager->settle();
}


static Message* encode(const UPID& from,
                       const UPID& to,
                       const string& name,
                       const string& data = "")
{
  Message* message = new Message();
  message->from = from;
  message->to = to;
  message->name = name;
  message->body = data;
  return message;
}


static void transport(Message* message, ProcessBase* sender = NULL)
{
  if (message->to.address == __address__) {
    // Local message.
    process_manager->deliver(message->to, new MessageEvent(message), sender);
  } else {
    // Remote message.
    socket_manager->send(message);
  }
}


static bool libprocess(Request* request)
{
  return
    (request->method == "POST" &&
     request->headers.contains("User-Agent") &&
     request->headers["User-Agent"].find("libprocess/") == 0) ||
    (request->method == "POST" &&
     request->headers.contains("Libprocess-From"));
}


static Message* parse(Request* request)
{
  // TODO(benh): Do better error handling (to deal with a malformed
  // libprocess message, malicious or otherwise).

  // First try and determine 'from'.
  Option<UPID> from = None();

  if (request->headers.contains("Libprocess-From")) {
    from = UPID(strings::trim(request->headers["Libprocess-From"]));
  } else {
    // Try and get 'from' from the User-Agent.
    const string& agent = request->headers["User-Agent"];
    const string identifier = "libprocess/";
    size_t index = agent.find(identifier);
    if (index != string::npos) {
      from = UPID(agent.substr(index + identifier.size(), agent.size()));
    }
  }

  if (from.isNone()) {
    return NULL;
  }

  // Now determine 'to'.
  size_t index = request->path.find('/', 1);
  index = index != string::npos ? index - 1 : string::npos;

  // Decode possible percent-encoded 'to'.
  Try<string> decode = http::decode(request->path.substr(1, index));

  if (decode.isError()) {
    VLOG(2) << "Failed to decode URL path: " << decode.get();
    return NULL;
  }

  const UPID to(decode.get(), __address__);

  // And now determine 'name'.
  index = index != string::npos ? index + 2: request->path.size();
  const string name = request->path.substr(index);

  VLOG(2) << "Parsed message name '" << name
          << "' for " << to << " from " << from.get();

  Message* message = new Message();
  message->name = name;
  message->from = from.get();
  message->to = to;
  message->body = request->body;

  return message;
}


namespace internal {

void decode_recv(
    const Future<size_t>& length,
    char* data,
    size_t size,
    Socket* socket,
    DataDecoder* decoder)
{
  if (length.isDiscarded() || length.isFailed()) {
    if (length.isFailed()) {
      VLOG(1) << "Decode failure: " << length.failure();
    }

    socket_manager->close(*socket);
    delete[] data;
    delete decoder;
    delete socket;
    return;
  }

  if (length.get() == 0) {
    socket_manager->close(*socket);
    delete[] data;
    delete decoder;
    delete socket;
    return;
  }

  // Decode as much of the data as possible into HTTP requests.
  const deque<Request*> requests = decoder->decode(data, length.get());

  if (!requests.empty()) {
    foreach (Request* request, requests) {
      // Augment each Request with the client's address. This should
      // never fail since there remains a reference to this Socket!
      Try<Address> address = socket->address();
      CHECK_SOME(address);
      request->client = address.get();
      process_manager->handle(decoder->socket(), request);
    }
  } else if (requests.empty() && decoder->failed()) {
    VLOG(1) << "Decoder error while receiving";
    socket_manager->close(*socket);
    delete[] data;
    delete decoder;
    delete socket;
    return;
  }

  socket->recv(data, size)
    .onAny(lambda::bind(&decode_recv, lambda::_1, data, size, socket, decoder));
}

} // namespace internal {


void* schedule(void* arg)
{
  do {
    ProcessBase* process = process_manager->dequeue();
    if (process == NULL) {
      Gate::state_t old = gate->approach();
      process = process_manager->dequeue();
      if (process == NULL) {
        gate->arrive(old); // Wait at gate if idle.
        continue;
      } else {
        gate->leave();
      }
    }
    process_manager->resume(process);
  } while (true);
}


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
//   sigaction(signal, &sa, NULL);
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
    memset(data, 0, size);

    DataDecoder* decoder = new DataDecoder(socket.get());

    socket.get().recv(data, size)
      .onAny(lambda::bind(
          &internal::decode_recv,
          lambda::_1,
          data,
          size,
          new Socket(socket.get()),
          decoder));
  }

  __s__->accept()
    .onAny(lambda::bind(&on_accept, lambda::_1));
}

} // namespace internal {


namespace firewall {

void install(std::vector<Owned<FirewallRule>>&& rules)
{
  process::initialize();

  process_manager->installFirewall(std::move(rules));
}

} // namespace firewall {

void initialize(const string& delegate)
{
  // TODO(benh): Return an error if attempting to initialize again
  // with a different delegate then originally specified.

  // static pthread_once_t init = PTHREAD_ONCE_INIT;
  // pthread_once(&init, ...);

  static volatile bool initialized = false;
  static volatile bool initializing = true;

  // Try and do the initialization or wait for it to complete.
  if (initialized && !initializing) {
    return;
  } else if (initialized && initializing) {
    while (initializing);
    return;
  } else {
    if (!__sync_bool_compare_and_swap(&initialized, false, true)) {
      while (initializing);
      return;
    }
  }

//   // Install signal handler.
//   struct sigaction sa;

//   sa.sa_handler = (void (*) (int)) sigbad;
//   sigemptyset (&sa.sa_mask);
//   sa.sa_flags = SA_RESTART;

//   sigaction (SIGTERM, &sa, NULL);
//   sigaction (SIGINT, &sa, NULL);
//   sigaction (SIGQUIT, &sa, NULL);
//   sigaction (SIGSEGV, &sa, NULL);
//   sigaction (SIGILL, &sa, NULL);
// #ifdef SIGBUS
//   sigaction (SIGBUS, &sa, NULL);
// #endif
// #ifdef SIGSTKFLT
//   sigaction (SIGSTKFLT, &sa, NULL);
// #endif
//   sigaction (SIGABRT, &sa, NULL);

//   sigaction (SIGFPE, &sa, NULL);

#ifdef __sun__
  /* Need to ignore this since we can't do MSG_NOSIGNAL on Solaris. */
  signal(SIGPIPE, SIG_IGN);
#endif // __sun__

  // Create a new ProcessManager and SocketManager.
  process_manager = new ProcessManager(delegate);
  socket_manager = new SocketManager();

  // Setup processing threads.
  // We create no fewer than 8 threads because some tests require
  // more worker threads than 'sysconf(_SC_NPROCESSORS_ONLN)' on
  // computers with fewer cores.
  // e.g. https://issues.apache.org/jira/browse/MESOS-818
  //
  // TODO(xujyan): Use a smarter algorithm to allocate threads.
  // Allocating a static number of threads can cause starvation if
  // there are more waiting Processes than the number of worker
  // threads.
  long cpus = std::max(8L, sysconf(_SC_NPROCESSORS_ONLN));

  for (int i = 0; i < cpus; i++) {
    pthread_t thread; // For now, not saving handles on our threads.
    if (pthread_create(&thread, NULL, schedule, NULL) != 0) {
      LOG(FATAL) << "Failed to initialize, pthread_create";
    }
  }

  // Initialize the event loop.
  EventLoop::initialize();
  Clock::initialize(lambda::bind(&timedout, lambda::_1));

//   ev_child_init(&child_watcher, child_exited, pid, 0);
//   ev_child_start(loop, &cw);

//   /* Install signal handler. */
//   struct sigaction sa;

//   sa.sa_handler = ev_sighandler;
//   sigfillset (&sa.sa_mask);
//   sa.sa_flags = SA_RESTART; /* if restarting works we save one iteration */
//   sigaction (w->signum, &sa, 0);

//   sigemptyset (&sa.sa_mask);
//   sigaddset (&sa.sa_mask, w->signum);
//   sigprocmask (SIG_UNBLOCK, &sa.sa_mask, 0);

  pthread_t thread; // For now, not saving handles on our threads.
  if (pthread_create(&thread, NULL, &EventLoop::run, NULL) != 0) {
    LOG(FATAL) << "Failed to initialize, pthread_create";
  }

  Address address_to_bind = Address::LOCALHOST_ANY();

  // Check environment for ip.
  Option<string> value = os::getenv("LIBPROCESS_IP");
  if (value.isSome()) {
    Try<net::IP> ip = net::IP::parse(value.get(), AF_INET);
    if (ip.isError()) {
      LOG(FATAL) << "Parsing LIBPROCESS_IP=" << value.get()
                 << " failed: " << ip.error();
    }
    address_to_bind.ip = ip.get();
  }

  // Check environment for port.
  value = os::getenv("LIBPROCESS_PORT");
  if (value.isSome()) {
    const int result = atoi(value.get().c_str());
    if (result < 0 || result > USHRT_MAX) {
      LOG(FATAL) << "LIBPROCESS_PORT=" << value.get() << " is not a valid port";
    }
    address_to_bind.port = result;
  }

  // Create a "server" socket for communicating.
  Try<Socket> create = Socket::create();
  if (create.isError()) {
    PLOG(FATAL) << "Failed to construct server socket:" << create.error();
  }
  __s__ = new Socket(create.get());

  // Allow address reuse.
  int on = 1;
  if (setsockopt(__s__->get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    PLOG(FATAL) << "Failed to initialize, setsockopt(SO_REUSEADDR)";
  }

  Try<Address> bind = __s__->bind(address_to_bind);
  if (bind.isError()) {
    PLOG(FATAL) << "Failed to initialize: " << bind.error();
  }

  __address__ = bind.get();

  // If public ip and port are present, use them instead
  value = getenv("LIBPROCESS_PUBLIC_IP");
  if (value.isSome()) {
    Try<net::IP> ip = net::IP::parse(value.get(), AF_INET);
    if (ip.isError()) {
      LOG(FATAL) << "Parsing LIBPROCESS_PUBLIC_IP=" << value.get()
                 << " failed: " << ip.error();
    }
    __address__.ip = ip.get();
  }

  value = getenv("LIBPROCESS_PUBLIC_PORT");
  if (value.isSome()) {
    int result = atoi(value.get().c_str());
    if (result < 0 || result > USHRT_MAX) {
      LOG(FATAL) << "LIBPROCESS_PUBLIC_PORT=" << value.get() << " is not a valid port";
    }
    __address__.port = result;
  }

  // Lookup hostname if missing ip or if ip is 0.0.0.0 in case we
  // actually have a valid external ip address. Note that we need only
  // one ip address, so that other processes can send and receive and
  // don't get confused as to whom they are sending to.
  if (__address__.ip.isAny()) {
    char hostname[512];

    if (gethostname(hostname, sizeof(hostname)) < 0) {
      LOG(FATAL) << "Failed to initialize, gethostname: "
                 << hstrerror(h_errno);
    }

    // Lookup IP address of local hostname.
    Try<net::IP> ip = net::getIP(hostname, __address__.ip.family());

    if (ip.isError()) {
      LOG(FATAL) << ip.error();
    }

    __address__.ip = ip.get();
  }

  Try<Nothing> listen = __s__->listen(LISTEN_BACKLOG);
  if (listen.isError()) {
    PLOG(FATAL) << "Failed to initialize: " << listen.error();
  }

  // Need to set initialzing here so that we can actually invoke
  // 'spawn' below for the garbage collector.
  initializing = false;

  __s__->accept()
    .onAny(lambda::bind(&internal::on_accept, lambda::_1));

  // TODO(benh): Make sure creating the garbage collector, logging
  // process, and profiler always succeeds and use supervisors to make
  // sure that none terminate.

  // Create global garbage collector process.
  gc = spawn(new GarbageCollector());

  // Create global help process.
  help = spawn(new Help(), true);

  // Create the global logging process.
  spawn(new Logging(), true);

  // Create the global profiler process.
  spawn(new Profiler(), true);

  // Create the global system statistics process.
  spawn(new System(), true);

  // Create the global statistics.
  // TODO(dhamon): Plumb this through to metrics.
  // value = os::getenv("LIBPROCESS_STATISTICS_WINDOW");
  // if (value.isSome()) {
  //   Try<Duration> window = Duration::parse(value.get());
  //   if (window.isError()) {
  //     LOG(FATAL) << "LIBPROCESS_STATISTICS_WINDOW=" << value.get()
  //                << " is not a valid duration: " << window.error();
  //   }
  //   statistics = new Statistics(window.get());
  // } else {
  //   // TODO(bmahler): Investigate memory implications of this window
  //   // size. We may also want to provide a maximum memory size rather than
  //   // time window. Or, offload older data to disk, etc.
  //   statistics = new Statistics(LIBPROCESS_STATISTICS_WINDOW);
  // }

  // Ensure metrics process is running.
  // TODO(bmahler): Consider initializing this consistently with
  // the other global Processes.
  MetricsProcess* metricsProcess = MetricsProcess::instance();
  CHECK_NOTNULL(metricsProcess);

  // Initialize the mime types.
  mime::initialize();

  // Initialize the response statuses.
  http::initialize();

  // Add a route for getting process information.
  lambda::function<Future<Response>(const Request&)> __processes__ =
    lambda::bind(&ProcessManager::__processes__, process_manager, lambda::_1);

  new Route("/__processes__", None(), __processes__);

  VLOG(1) << "libprocess is initialized on " << address() << " for " << cpus
          << " cpus";
}


void finalize()
{
  delete process_manager;

  // TODO(benh): Finialize/shutdown Clock so that it doesn't attempt
  // to dereference 'process_manager' in the 'timedout' callback.
}


Address address()
{
  process::initialize();
  return __address__;
}


HttpProxy::HttpProxy(const Socket& _socket)
  : ProcessBase(ID::generate("__http__")),
    socket(_socket) {}


HttpProxy::~HttpProxy()
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
    item->future->discard();

    // But it might have already been ready. In general, we need to
    // wait until this future is potentially ready in order to attempt
    // to close a pipe if one exists.
    item->future->onReady([](const Response& response) {
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
}


void HttpProxy::enqueue(const Response& response, const Request& request)
{
  handle(new Future<Response>(response), request);
}


void HttpProxy::handle(Future<Response>* future, const Request& request)
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
    items.front()->future->onAny(
        defer(self(), &HttpProxy::waited, lambda::_1));
  }
}


void HttpProxy::waited(const Future<Response>& future)
{
  CHECK(items.size() > 0);
  Item* item = items.front();

  CHECK(future == *item->future);

  // Process the item and determine if we're done or not (so we know
  // whether to start waiting on the next responses).
  bool processed = process(*item->future, item->request);

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
    socket_manager->send(ServiceUnavailable(), request, socket);
    return true; // All done, can process next response.
  }

  Response response = future.get();

  // If the response specifies a path, try and perform a sendfile.
  if (response.type == Response::PATH) {
    // Make sure no body is sent (this is really an error and
    // should be reported and no response sent.
    response.body.clear();

    const string& path = response.path;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errno == ENOENT || errno == ENOTDIR) {
          VLOG(1) << "Returning '404 Not Found' for path '" << path << "'";
          socket_manager->send(NotFound(), request, socket);
      } else {
        const char* error = strerror(errno);
        VLOG(1) << "Failed to send file at '" << path << "': " << error;
        socket_manager->send(InternalServerError(), request, socket);
      }
    } else {
      struct stat s; // Need 'struct' because of function named 'stat'.
      if (fstat(fd, &s) != 0) {
        const char* error = strerror(errno);
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
            new HttpResponseEncoder(socket, response, request),
            true);

        // Note the file descriptor gets closed by FileEncoder.
        socket_manager->send(
            new FileEncoder(socket, fd, s.st_size),
            request.keepAlive);
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
        new HttpResponseEncoder(socket, response, request),
        true);

    CHECK_SOME(response.reader);
    http::Pipe::Reader reader = response.reader.get();

    pipe = reader;

    reader.read()
      .onAny(defer(self(), &Self::stream, request, lambda::_1));

    return false; // Streaming, don't process next response (yet)!
  } else {
    socket_manager->send(response, request, socket);
  }

  return true; // All done, can process next response.
}


void HttpProxy::stream(
    const Request& request,
    const Future<string>& chunk)
{
  CHECK_SOME(pipe);

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
        new DataEncoder(socket, out.str()),
        finished ? request.keepAlive : true);
  } else if (chunk.isFailed()) {
    VLOG(1) << "Failed to read from stream: " << chunk.failure();
    // TODO(bmahler): Have to close connection if headers were sent!
    socket_manager->send(InternalServerError(), request, socket);
    finished = true;
  } else {
    VLOG(1) << "Failed to read from stream: discarded";
    // TODO(bmahler): Have to close connection if headers were sent!
    socket_manager->send(InternalServerError(), request, socket);
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


void SocketManager::accepted(const Socket& socket)
{
  synchronized (mutex) {
    sockets[socket] = new Socket(socket);
  }
}


namespace internal {

void ignore_recv_data(
    const Future<size_t>& length,
    Socket* socket,
    char* data,
    size_t size)
{
  if (length.isDiscarded() || length.isFailed()) {
    socket_manager->close(*socket);
    delete[] data;
    delete socket;
    return;
  }

  if (length.get() == 0) {
    socket_manager->close(*socket);
    delete[] data;
    delete socket;
    return;
  }

  socket->recv(data, size)
    .onAny(lambda::bind(&ignore_recv_data, lambda::_1, socket, data, size));
}


// Forward declaration.
void send(Encoder* encoder, Socket* socket);


void link_connect(const Future<Nothing>& future, Socket* socket)
{
  if (future.isDiscarded() || future.isFailed()) {
    if (future.isFailed()) {
      VLOG(1) << "Failed to link, connect: " << future.failure();
    }
    socket_manager->close(*socket);
    delete socket;
    return;
  }

  size_t size = 80 * 1024;
  char* data = new char[size];

  socket->recv(data, size)
    .onAny(lambda::bind(
        &ignore_recv_data,
        lambda::_1,
        socket,
        data,
        size));

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
  Encoder* encoder = socket_manager->next(*socket);

  if (encoder != NULL) {
    send(encoder, new Socket(*socket));
  }
}


} // namespace internal {


void SocketManager::link(ProcessBase* process, const UPID& to)
{
  // TODO(benh): The semantics we want to support for link are such
  // that if there is nobody to link to (local or remote) then an
  // ExitedEvent gets generated. This functionality has only been
  // implemented when the link is local, not remote. Of course, if
  // there is nobody listening on the remote side, then this should
  // work remotely ... but if there is someone listening remotely just
  // not at that id, then it will silently continue executing.

  CHECK(process != NULL);

  Option<Socket> socket = None();
  bool connect = false;

  synchronized (mutex) {
    // Check if the socket address is remote and there isn't a persistant link.
    if (to.address != __address__  && persists.count(to.address) == 0) {
      // Okay, no link, let's create a socket.
      Try<Socket> create = Socket::create();
      if (create.isError()) {
        VLOG(1) << "Failed to link, create socket: " << create.error();
        return;
      }
      socket = create.get();
      int s = socket.get().get();

      sockets[s] = new Socket(socket.get());
      addresses[s] = to.address;

      persists[to.address] = s;

      // Initialize 'outgoing' to prevent a race with
      // SocketManager::send() while the socket is not yet connected.
      // Initializing the 'outgoing' queue prevents
      // SocketManager::send() from trying to write before it's
      // connected.
      outgoing[s];

      connect = true;
    }

    links.linkers[to].insert(process);
    links.linkees[process].insert(to);
    if (to.address != __address__) {
      links.remotes[to.address].insert(to);
    }
  }

  if (connect) {
    CHECK_SOME(socket);
    Socket(socket.get()).connect(to.address) // Copy to drop const.
      .onAny(lambda::bind(
          &internal::link_connect,
          lambda::_1,
          new Socket(socket.get())));
  }
}


PID<HttpProxy> SocketManager::proxy(const Socket& socket)
{
  HttpProxy* proxy = NULL;

  synchronized (mutex) {
    // This socket might have been asked to get closed (e.g., remote
    // side hang up) while a process is attempting to handle an HTTP
    // request. Thus, if there is no more socket, return an empty PID.
    if (sockets.count(socket) > 0) {
      if (proxies.count(socket) > 0) {
        return proxies[socket]->self();
      } else {
        proxy = new HttpProxy(*sockets[socket]);
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
  if (proxy != NULL) {
    return spawn(proxy, true);
  }

  return PID<HttpProxy>();
}


namespace internal {

void _send(
    const Future<size_t>& result,
    Socket* socket,
    Encoder* encoder,
    size_t size);


void send(Encoder* encoder, Socket* socket)
{
  switch (encoder->kind()) {
    case Encoder::DATA: {
      size_t size;
      const char* data = reinterpret_cast<DataEncoder*>(encoder)->next(&size);
      socket->send(data, size)
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
      int fd = reinterpret_cast<FileEncoder*>(encoder)->next(&offset, &size);
      socket->sendfile(fd, offset, size)
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
    Socket* socket,
    Encoder* encoder,
    size_t size)
{
  if (length.isDiscarded() || length.isFailed()) {
    socket_manager->close(*socket);
    delete socket;
    delete encoder;
  } else {
    // Update the encoder with the amount sent.
    encoder->backup(size - length.get());

    // See if there is any more of the message to send.
    if (encoder->remaining() == 0) {
      delete encoder;

      // Check for more stuff to send on socket.
      Encoder* next = socket_manager->next(*socket);
      if (next != NULL) {
        send(next, socket);
      } else {
        delete socket;
      }
    } else {
      send(encoder, socket);
    }
  }
}

} // namespace internal {


void SocketManager::send(Encoder* encoder, bool persist)
{
  CHECK(encoder != NULL);

  synchronized (mutex) {
    Socket socket = encoder->socket();
    if (sockets.count(socket) > 0) {
      // Update whether or not this socket should get disposed after
      // there is no more data to send.
      if (!persist) {
        dispose.insert(socket);
      }

      if (outgoing.count(socket) > 0) {
        outgoing[socket].push(encoder);
        encoder = NULL;
      } else {
        // Initialize the outgoing queue.
        outgoing[socket];
      }
    } else {
      VLOG(1) << "Attempting to send on a no longer valid socket!";
      delete encoder;
      encoder = NULL;
    }
  }

  if (encoder != NULL) {
    internal::send(encoder, new Socket(encoder->socket()));
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

  send(new HttpResponseEncoder(socket, response, request), persist);
}


namespace internal {

void send_connect(
    const Future<Nothing>& future,
    Socket* socket,
    Message* message)
{
  if (future.isDiscarded() || future.isFailed()) {
    if (future.isFailed()) {
      VLOG(1) << "Failed to send '" << message->name << "' to '"
              << message->to.address << "', connect: " << future.failure();
    }
    socket_manager->close(*socket);
    delete socket;
    delete message;
    return;
  }

  Encoder* encoder = new MessageEncoder(*socket, message);

  // Receive and ignore data from this socket. Note that we don't
  // expect to receive anything other than HTTP '202 Accepted'
  // responses which we just ignore.
  size_t size = 80 * 1024;
  char* data = new char[size];

  socket->recv(data, size)
    .onAny(lambda::bind(
        &ignore_recv_data,
        lambda::_1,
        new Socket(*socket),
        data,
        size));

  internal::send(encoder, socket);
}

} // namespace internal {


void SocketManager::send(Message* message)
{
  CHECK(message != NULL);

  const Address& address = message->to.address;

  Option<Socket> socket = None();
  bool connect = false;

  synchronized (mutex) {
    // Check if there is already a socket.
    bool persist = persists.count(address) > 0;
    bool temp = temps.count(address) > 0;
    if (persist || temp) {
      int s = persist ? persists[address] : temps[address];
      CHECK(sockets.count(s) > 0);
      socket = *sockets[s];

      // Update whether or not this socket should get disposed after
      // there is no more data to send.
      if (!persist) {
        dispose.insert(socket.get());
      }

      if (outgoing.count(socket.get()) > 0) {
        outgoing[socket.get()].push(new MessageEncoder(socket.get(), message));
        return;
      } else {
        // Initialize the outgoing queue.
        outgoing[socket.get()];
      }

    } else {
      // No peristent or temporary socket to the socket address
      // currently exists, so we create a temporary one.
      Try<Socket> create = Socket::create();
      if (create.isError()) {
        VLOG(1) << "Failed to send, create socket: " << create.error();
        delete message;
        return;
      }
      socket = create.get();
      int s = socket.get();

      sockets[s] = new Socket(socket.get());
      addresses[s] = address;
      temps[address] = s;

      dispose.insert(s);

      // Initialize the outgoing queue.
      outgoing[s];

      connect = true;
    }
  }

  if (connect) {
    CHECK_SOME(socket);
    Socket(socket.get()).connect(address) // Copy to drop const.
      .onAny(lambda::bind(
          &internal::send_connect,
          lambda::_1,
          new Socket(socket.get()),
          message));
  } else {
    // If we're not connecting and we haven't added the encoder to
    // the 'outgoing' queue then schedule it to be sent.
    internal::send(
        new MessageEncoder(socket.get(), message),
        new Socket(socket.get()));
  }
}


Encoder* SocketManager::next(int s)
{
  HttpProxy* proxy = NULL; // Non-null if needs to be terminated.

  synchronized (mutex) {
    // We cannot assume 'sockets.count(s) > 0' here because it's
    // possible that 's' has been removed with a a call to
    // SocketManager::close. For example, it could be the case that a
    // socket has gone to CLOSE_WAIT and the call to read in
    // io::read returned 0 causing SocketManager::close to get
    // invoked. Later a call to 'send' or 'sendfile' (e.g., in
    // send_data or send_file) can "succeed" (because the socket is
    // not "closed" yet because there are still some Socket
    // references, namely the reference being used in send_data or
    // send_file!). However, when SocketManger::next is actually
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
          if (addresses.count(s) > 0) {
            const Address& address = addresses[s];
            CHECK(temps.count(address) > 0 && temps[address] == s);
            temps.erase(address);
            addresses.erase(s);
          }

          if (proxies.count(s) > 0) {
            proxy = proxies[s];
            proxies.erase(s);
          }

          dispose.erase(s);
          auto iterator = sockets.find(s);
          delete iterator->second;
          sockets.erase(iterator);

          // We don't actually close the socket (we wait for the Socket
          // abstraction to close it once there are no more references),
          // but we do shutdown the receiving end so any DataDecoder
          // will get cleaned up (which might have the last reference).
          shutdown(s, SHUT_RD);
        }
      }
    }
  }

  // We terminate the proxy outside the synchronized block to avoid
  // possible deadlock between the ProcessManager and SocketManager
  // (see comment in SocketManager::proxy for more information).
  if (proxy != NULL) {
    terminate(proxy);
  }

  return NULL;
}


void SocketManager::close(int s)
{
  HttpProxy* proxy = NULL; // Non-null if needs to be terminated.

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
      if (addresses.count(s) > 0) {
        const Address& address = addresses[s];

        // Don't bother invoking exited unless socket was persistant.
        if (persists.count(address) > 0 && persists[address] == s) {
          persists.erase(address);
          exited(address); // Generate ExitedEvent(s)!
        } else if (temps.count(address) > 0 && temps[address] == s) {
          temps.erase(address);
        }

        addresses.erase(s);
      }

      // Clean up any proxy associated with this socket.
      if (proxies.count(s) > 0) {
        proxy = proxies[s];
        proxies.erase(s);
      }

      // We need to stop any 'ignore_data' receivers as they may have
      // the last Socket reference so we shutdown recvs but don't do a
      // full close (since that will be taken care of by ~Socket, see
      // comment below). Calling 'shutdown' will trigger 'ignore_data'
      // which will get back a 0 (i.e., EOF) when it tries to 'recv'
      // from the socket. Note we need to do this before we call
      // 'sockets.erase(s)' to avoid the potential race with the last
      // reference being in 'sockets'.
      shutdown(s, SHUT_RD);

      dispose.erase(s);
      auto iterator = sockets.find(s);
      delete iterator->second;
      sockets.erase(iterator);
    }
  }

  // We terminate the proxy outside the synchronized block to avoid
  // possible deadlock between the ProcessManager and SocketManager.
  if (proxy != NULL) {
    terminate(proxy);
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


ProcessManager::ProcessManager(const string& _delegate)
  : delegate(_delegate)
{
  running = 0;
  __sync_synchronize(); // Ensure write to 'running' visible in other threads.
}


ProcessManager::~ProcessManager()
{
  ProcessBase* process = NULL;
  // Pop a process off the top and terminate it. Don't hold the lock
  // or process the whole map as terminating one process might
  // trigger other terminations. Deal with them one at a time.
  do {
    synchronized (processes_mutex) {
      process = !processes.empty() ? processes.begin()->second : NULL;
    }
    if (process != NULL) {
      process::terminate(process);
      process::wait(process);
    }
  } while (process != NULL);
}


ProcessReference ProcessManager::use(const UPID& pid)
{
  if (pid.address == __address__) {
    synchronized (processes_mutex) {
      if (processes.count(pid.id) > 0) {
        // Note that the ProcessReference constructor _must_ get
        // called while holding the lock on processes so that waiting
        // for references is atomic (i.e., race free).
        return ProcessReference(processes[pid.id]);
      }
    }
  }

  return ProcessReference(NULL);
}


bool ProcessManager::handle(
    const Socket& socket,
    Request* request)
{
  CHECK(request != NULL);

  // Check if this is a libprocess request (i.e., 'User-Agent:
  // libprocess/id@ip:port') and if so, parse as a message.
  if (libprocess(request)) {
    Message* message = parse(request);
    if (message != NULL) {
      // TODO(benh): Use the sender PID when delivering in order to
      // capture happens-before timing relationships for testing.
      bool accepted = deliver(message->to, new MessageEvent(message));

      // Get the HttpProxy pid for this socket.
      PID<HttpProxy> proxy = socket_manager->proxy(socket);

      // Only send back an HTTP response if this isn't from libprocess
      // (which we determine by looking at the User-Agent). This is
      // necessary because older versions of libprocess would try and
      // recv the data and parse it as an HTTP request which would
      // fail thus causing the socket to get closed (but now
      // libprocess will ignore responses, see ignore_data).
      Option<string> agent = request->headers.get("User-Agent");
      if (agent.get("").find("libprocess/") == string::npos) {
        if (accepted) {
          VLOG(2) << "Accepted libprocess message to " << request->path;
          dispatch(proxy, &HttpProxy::enqueue, Accepted(), *request);
        } else {
          VLOG(1) << "Failed to handle libprocess message to "
                  << request->path << ": not found";
          dispatch(proxy, &HttpProxy::enqueue, NotFound(), *request);
        }
      }

      delete request;

      return accepted;
    }

    VLOG(1) << "Failed to handle libprocess message: "
            << request->method << " " << request->path
            << " (User-Agent: " << request->headers["User-Agent"] << ")";

    delete request;
    return false;
  }

  // Treat this as an HTTP request. Start by checking that the path
  // starts with a '/' (since the code below assumes as much).
  if (request->path.find('/') != 0) {
    VLOG(1) << "Returning '400 Bad Request' for '" << request->path << "'";

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(socket);

    // Enqueue the response with the HttpProxy so that it respects the
    // order of requests to account for HTTP/1.1 pipelining.
    dispatch(proxy, &HttpProxy::enqueue, BadRequest(), *request);

    // Cleanup request.
    delete request;
    return false;
  }

  // Ignore requests with relative paths (i.e., contain "/..").
  if (request->path.find("/..") != string::npos) {
    VLOG(1) << "Returning '404 Not Found' for '" << request->path
            << "' (ignoring requests with relative paths)";

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(socket);

    // Enqueue the response with the HttpProxy so that it respects the
    // order of requests to account for HTTP/1.1 pipelining.
    dispatch(proxy, &HttpProxy::enqueue, NotFound(), *request);

    // Cleanup request.
    delete request;
    return false;
  }

  synchronized (firewall_mutex) {
    foreach (Owned<FirewallRule>& rule, firewallRules) {
      Try<Nothing> applied = rule->apply(socket, *request);
      if (applied.isError()) {
        VLOG(1) << "Returning '403 Forbidden' for '" << request->path
                << "' (firewall rule forbids connection): "
                << applied.error();
        // TODO(arojas): Get rid of the duplicated code to return an
        // error.

        // Get the HttpProxy pid for this socket.
        PID<HttpProxy> proxy = socket_manager->proxy(socket);

        // Enqueue the response with the HttpProxy so that it respects
        // the order of requests to account for HTTP/1.1 pipelining.
        dispatch(
            proxy, &HttpProxy::enqueue, Forbidden(applied.error()), *request);

        // Cleanup request.
        delete request;
        return false;
      }
    }
  }

  // Split the path by '/'.
  vector<string> tokens = strings::tokenize(request->path, "/");

  // Try and determine a receiver, otherwise try and delegate.
  ProcessReference receiver;

  if (tokens.size() == 0 && delegate != "") {
    request->path = "/" + delegate;
    receiver = use(UPID(delegate, __address__));
  } else if (tokens.size() > 0) {
    // Decode possible percent-encoded path.
    Try<string> decode = http::decode(tokens[0]);
    if (!decode.isError()) {
      receiver = use(UPID(decode.get(), __address__));
    } else {
      VLOG(1) << "Failed to decode URL path: " << decode.error();
    }
  }

  if (!receiver && delegate != "") {
    // Try and delegate the request.
    request->path = "/" + delegate + request->path;
    receiver = use(UPID(delegate, __address__));
  }

  if (receiver) {
    // TODO(benh): Use the sender PID in order to capture
    // happens-before timing relationships for testing.
    return deliver(receiver, new HttpEvent(socket, request));
  }

  // This has no receiver, send error response.
  VLOG(1) << "Returning '404 Not Found' for '" << request->path << "'";

  // Get the HttpProxy pid for this socket.
  PID<HttpProxy> proxy = socket_manager->proxy(socket);

  // Enqueue the response with the HttpProxy so that it respects the
  // order of requests to account for HTTP/1.1 pipelining.
  dispatch(proxy, &HttpProxy::enqueue, NotFound(), *request);

  // Cleanup request.
  delete request;
  return false;
}


bool ProcessManager::deliver(
    ProcessBase* receiver,
    Event* event,
    ProcessBase* sender)
{
  CHECK(event != NULL);

  // If we are using a manual clock then update the current time of
  // the receiver using the sender if necessary to preserve the
  // happens-before relationship between the sender and receiver. Note
  // that the assumption is that the sender remains valid for at least
  // the duration of this routine (so that we can look up it's current
  // time).
  if (Clock::paused()) {
    Clock::update(receiver, Clock::now(sender != NULL ? sender : __process__));
  }

  receiver->enqueue(event);

  return true;
}


bool ProcessManager::deliver(
    const UPID& to,
    Event* event,
    ProcessBase* sender)
{
  CHECK(event != NULL);

  if (ProcessReference receiver = use(to)) {
    return deliver(receiver, event, sender);
  }
  VLOG(2) << "Dropping event for process " << to;

  delete event;
  return false;
}


UPID ProcessManager::spawn(ProcessBase* process, bool manage)
{
  CHECK(process != NULL);

  synchronized (processes_mutex) {
    if (processes.count(process->pid.id) > 0) {
      return UPID();
    } else {
      processes[process->pid.id] = process;
    }
  }

  // Use the garbage collector if requested.
  if (manage) {
    dispatch(gc, &GarbageCollector::manage<ProcessBase>, process);
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

  CHECK(process->state == ProcessBase::BOTTOM ||
        process->state == ProcessBase::READY);

  if (process->state == ProcessBase::BOTTOM) {
    process->state = ProcessBase::RUNNING;
    try { process->initialize(); }
    catch (...) { terminate = true; }
  }

  while (!terminate && !blocked) {
    Event* event = NULL;

    process->lock();
    {
      if (process->events.size() > 0) {
        event = process->events.front();
        process->events.pop_front();
        process->state = ProcessBase::RUNNING;
      } else {
        process->state = ProcessBase::BLOCKED;
        blocked = true;
      }
    }
    process->unlock();

    if (!blocked) {
      CHECK(event != NULL);

      // Determine if we should filter this event.
      synchronized (filterer_mutex) {
        if (filterer != NULL) {
          bool filter = false;
          struct FilterVisitor : EventVisitor
          {
            explicit FilterVisitor(bool* _filter) : filter(_filter) {}

            virtual void visit(const MessageEvent& event)
            {
              *filter = filterer->filter(event);
            }

            virtual void visit(const DispatchEvent& event)
            {
              *filter = filterer->filter(event);
            }

            virtual void visit(const HttpEvent& event)
            {
              *filter = filterer->filter(event);
            }

            virtual void visit(const ExitedEvent& event)
            {
              *filter = filterer->filter(event);
            }

            bool* filter;
          } visitor(&filter);

          event->visit(&visitor);

          if (filter) {
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

  __process__ = NULL;

  CHECK_GE(running, 1);
  __sync_fetch_and_sub(&running, 1);
}


void ProcessManager::cleanup(ProcessBase* process)
{
  VLOG(2) << "Cleaning up " << process->pid;

  // First, set the terminating state so no more events will get
  // enqueued and delete al the pending events. We want to delete the
  // events before we hold the processes lock because deleting an
  // event could cause code outside libprocess to get executed which
  // might cause a deadlock with the processes lock. Likewise,
  // deleting the events now rather than later has the nice property
  // of making sure that any events that might have gotten enqueued on
  // the process we are cleaning up will get dropped (since it's
  // terminating) and eliminates the potential of enqueueing them on
  // another process that gets spawned with the same PID.
  deque<Event*> events;

  process->lock();
  {
    process->state = ProcessBase::TERMINATING;
    events = process->events;
    process->events.clear();
  }
  process->unlock();

  // Delete pending events.
  while (!events.empty()) {
    Event* event = events.front();
    events.pop_front();
    delete event;
  }

  // Possible gate non-libprocess threads are waiting at.
  Gate* gate = NULL;

  // Remove process.
  synchronized (processes_mutex) {
    // Wait for all process references to get cleaned up.
    while (process->refs > 0) {
#if defined(__i386__) || defined(__x86_64__)
      asm ("pause");
#endif
      __sync_synchronize();
    }

    process->lock();
    {
      CHECK(process->events.empty());

      processes.erase(process->pid.id);

      // Lookup gate to wake up waiting threads.
      map<ProcessBase*, Gate*>::iterator it = gates.find(process);
      if (it != gates.end()) {
        gate = it->second;
        // N.B. The last thread that leaves the gate also free's it.
        gates.erase(it);
      }

      CHECK(process->refs == 0);
      process->state = ProcessBase::TERMINATED;
    }
    process->unlock();

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
    // and SocketManger::link would see that the processes doesn't
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
    if (gate != NULL) {
      gate->open();
    }
  }
}


void ProcessManager::link(ProcessBase* process, const UPID& to)
{
  // Check if the pid is local.
  if (to.address != __address__) {
    socket_manager->link(process, to);
  } else {
    // Since the pid is local we want to get a reference to it's
    // underlying process so that while we are invoking the link
    // manager we don't miss sending a possible ExitedEvent.
    if (ProcessReference _ = use(to)) {
      socket_manager->link(process, to);
    } else {
      // Since the pid isn't valid it's process must have already died
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
      Clock::update(process, Clock::now(sender != NULL ? sender : __process__));
    }

    if (sender != NULL) {
      process->enqueue(new TerminateEvent(sender->self()), inject);
    } else {
      process->enqueue(new TerminateEvent(UPID()), inject);
    }
  }
}


bool ProcessManager::wait(const UPID& pid)
{
  // We use a gate for waiters. A gate is single use. That is, a new
  // gate is created when the first thread shows up and wants to wait
  // for a process that currently has no gate. Once that process
  // exits, the last thread to leave the gate will also clean it
  // up. Note that a gate will never get more threads waiting on it
  // after it has been opened, since the process should no longer be
  // valid and therefore will not have an entry in 'processes'.

  Gate* gate = NULL;
  Gate::state_t old;

  ProcessBase* process = NULL; // Set to non-null if we donate thread.

  // Try and approach the gate if necessary.
  synchronized (processes_mutex) {
    if (processes.count(pid.id) > 0) {
      process = processes[pid.id];
      CHECK(process->state != ProcessBase::TERMINATED);

      // Check and see if a gate already exists.
      if (gates.find(process) == gates.end()) {
        gates[process] = new Gate();
      }

      gate = gates[process];
      old = gate->approach();

      // Check if it is runnable in order to donate this thread.
      if (process->state == ProcessBase::BOTTOM ||
          process->state == ProcessBase::READY) {
        synchronized (runq_mutex) {
          list<ProcessBase*>::iterator it =
            find(runq.begin(), runq.end(), process);
          if (it != runq.end()) {
            // Found it! Remove it from the run queue since we'll be
            // donating our thread and also increment 'running' before
            // leaving this 'runq' protected critical section so that
            // everyone that is waiting for the processes to settle
            // continue to wait (otherwise they could see nothing in
            // 'runq' and 'running' equal to 0 between when we exit
            // this critical section and increment 'running').
            runq.erase(it);
            __sync_fetch_and_add(&running, 1);
          } else {
            // Another thread has resumed the process ...
            process = NULL;
          }
        }
      } else {
        // Process is not runnable, so no need to donate ...
        process = NULL;
      }
    }
  }

  if (process != NULL) {
    VLOG(2) << "Donating thread to " << process->pid << " while waiting";
    ProcessBase* donator = __process__;
    process_manager->resume(process);
    __process__ = donator;
  }

  // TODO(benh): Donating only once may not be sufficient, so we might
  // still deadlock here ... perhaps warn if that's the case?

  // Now arrive at the gate and wait until it opens.
  if (gate != NULL) {
    gate->arrive(old);

    if (gate->empty()) {
      delete gate;
    }

    return true;
  }

  return false;
}


void ProcessManager::installFirewall(std::vector<Owned<FirewallRule>>&& rules)
{
  synchronized (firewall_mutex) {
    firewallRules = std::move(rules);
  }
}


void ProcessManager::enqueue(ProcessBase* process)
{
  CHECK(process != NULL);

  // TODO(benh): Check and see if this process has it's own thread. If
  // it does, push it on that threads runq, and wake up that thread if
  // it's not running. Otherwise, check and see which thread this
  // process was last running on, and put it on that threads runq.

  synchronized (runq_mutex) {
    CHECK(find(runq.begin(), runq.end(), process) == runq.end());
    runq.push_back(process);
  }

  // Wake up the processing thread if necessary.
  gate->open();
}


ProcessBase* ProcessManager::dequeue()
{
  // TODO(benh): Remove a process from this thread's runq. If there
  // are no processes to run, and this is not a dedicated thread, then
  // steal one from another threads runq.

  ProcessBase* process = NULL;

  synchronized (runq_mutex) {
    if (!runq.empty()) {
      process = runq.front();
      runq.pop_front();
      // Increment the running count of processes in order to support
      // the Clock::settle() operation (this must be done atomically
      // with removing the process from the runq).
      __sync_fetch_and_add(&running, 1);
    }
  }

  return process;
}


void ProcessManager::settle()
{
  bool done = true;
  do {
    // While refactoring in order to isolate libev behind abstractions
    // it became evident that this os::sleep is vital for tests to
    // pass. In particular, there are certain tests that assume too
    // much before they attempt to do a settle. One such example is
    // tests doing http::get followed by Clock::settle, where they
    // expect the http::get will have properly enqueued a process on
    // the run queue but http::get is just sending bytes on a
    // socket. Without sleeping at the beginning of this function we
    // can get unlucky and appear settled when in actuallity the
    // kernel just hasn't copied the bytes to a socket or we haven't
    // yet read the bytes and enqueued an event on a process (and the
    // process on the run queue).
    os::sleep(Milliseconds(10));

    done = true; // Assume to start that we are settled.

    synchronized (runq_mutex) {
      if (!runq.empty()) {
        done = false;
        continue;
      }

      // Read barrier for 'running'.
      __sync_synchronize();

      if (running > 0) {
        done = false;
        continue;
      }

      if (!Clock::settled()) {
        done = false;
        continue;
      }
    }
  } while (!done);
}


Future<Response> ProcessManager::__processes__(const Request&)
{
  JSON::Array array;

  synchronized (processes_mutex) {
    foreachvalue (ProcessBase* process, process_manager->processes) {
      JSON::Object object;
      object.values["id"] = process->pid.id;

      JSON::Array events;

      struct JSONVisitor : EventVisitor
      {
        explicit JSONVisitor(JSON::Array* _events) : events(_events) {}

        virtual void visit(const MessageEvent& event)
        {
          JSON::Object object;
          object.values["type"] = "MESSAGE";

          const Message& message = *event.message;

          object.values["name"] = message.name;
          object.values["from"] = string(message.from);
          object.values["to"] = string(message.to);
          object.values["body"] = message.body;

          events->values.push_back(object);
        }

        virtual void visit(const HttpEvent& event)
        {
          JSON::Object object;
          object.values["type"] = "HTTP";

          const Request& request = *event.request;

          object.values["method"] = request.method;
          object.values["url"] = request.url;

          events->values.push_back(object);
        }

        virtual void visit(const DispatchEvent& event)
        {
          JSON::Object object;
          object.values["type"] = "DISPATCH";
          events->values.push_back(object);
        }

        virtual void visit(const ExitedEvent& event)
        {
          JSON::Object object;
          object.values["type"] = "EXITED";
          events->values.push_back(object);
        }

        virtual void visit(const TerminateEvent& event)
        {
          JSON::Object object;
          object.values["type"] = "TERMINATE";
          events->values.push_back(object);
        }

        JSON::Array* events;
      } visitor(&events);

      process->lock();
      {
        foreach (Event* event, process->events) {
          event->visit(&visitor);
        }
      }
      process->unlock();

      object.values["events"] = events;
      array.values.push_back(object);
    }
  }

  return OK(array);
}


ProcessBase::ProcessBase(const string& id)
{
  process::initialize();

  state = ProcessBase::BOTTOM;

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&m, &attr);
  pthread_mutexattr_destroy(&attr);

  refs = 0;

  pid.id = id != "" ? id : ID::generate();
  pid.address = __address__;

  // If using a manual clock, try and set current time of process
  // using happens before relationship between creator (__process__)
  // and createe (this)!
  if (Clock::paused()) {
    Clock::update(this, Clock::now(__process__), Clock::FORCE);
  }
}


ProcessBase::~ProcessBase() {}


void ProcessBase::enqueue(Event* event, bool inject)
{
  CHECK(event != NULL);

  lock();
  {
    if (state != TERMINATING && state != TERMINATED) {
      if (!inject) {
        events.push_back(event);
      } else {
        events.push_front(event);
      }

      if (state == BLOCKED) {
        state = READY;
        process_manager->enqueue(this);
      }

      CHECK(state == BOTTOM ||
            state == READY ||
            state == RUNNING);
    } else {
      delete event;
    }
  }
  unlock();
}


void ProcessBase::inject(
    const UPID& from,
    const string& name,
    const char* data,
    size_t length)
{
  if (!from)
    return;

  Message* message = encode(from, pid, name, string(data, length));

  enqueue(new MessageEvent(message), true);
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
  transport(encode(pid, to, name, string(data, length)), this);
}


void ProcessBase::visit(const MessageEvent& event)
{
  if (handlers.message.count(event.message->name) > 0) {
    handlers.message[event.message->name](
        event.message->from,
        event.message->body);
  } else if (delegates.count(event.message->name) > 0) {
    VLOG(1) << "Delegating message '" << event.message->name
            << "' to " << delegates[event.message->name];
    Message* message = new Message(*event.message);
    message->to = delegates[event.message->name];
    transport(message, this);
  }
}


void ProcessBase::visit(const DispatchEvent& event)
{
  (*event.f)(this);
}


void ProcessBase::visit(const HttpEvent& event)
{
  VLOG(1) << "Handling HTTP event for process '" << pid.id << "'"
          << " with path: '" << event.request->path << "'";

  CHECK(event.request->path.find('/') == 0); // See ProcessManager::handle.

  // Split the path by '/'.
  vector<string> tokens = strings::tokenize(event.request->path, "/");
  CHECK(tokens.size() >= 1);
  CHECK_EQ(pid.id, http::decode(tokens[0]).get());

  const string name = tokens.size() > 1 ? tokens[1] : "";

  if (handlers.http.count(name) > 0) {
    // Create the promise to link with whatever gets returned, as well
    // as a future to wait for the response.
    std::shared_ptr<Promise<Response>> promise(new Promise<Response>());

    Future<Response>* future = new Future<Response>(promise->future());

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(event.socket);

    // Let the HttpProxy know about this request (via the future).
    dispatch(proxy, &HttpProxy::handle, future, *event.request);

    // Now call the handler and associate the response with the promise.
    promise->associate(handlers.http[name](*event.request));
  } else if (assets.count(name) > 0) {
    OK response;
    response.type = Response::PATH;
    response.path = assets[name].path;

    // Construct the final path by appending remaining tokens.
    for (int i = 2; i < tokens.size(); i++) {
      response.path += "/" + tokens[i];
    }

    // Try and determine the Content-Type from an extension.
    Try<string> basename = os::basename(response.path);
    if (!basename.isError()) {
      size_t index = basename.get().find_last_of('.');
      if (index != string::npos) {
        string extension = basename.get().substr(index);
        if (assets[name].types.count(extension) > 0) {
          response.headers["Content-Type"] = assets[name].types[extension];
        }
      }
    }

    // TODO(benh): Use "text/plain" for assets that don't have an
    // extension or we don't have a mapping for? It might be better to
    // just let the browser guess (or do it's own default).

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(event.socket);

    // Enqueue the response with the HttpProxy so that it respects the
    // order of requests to account for HTTP/1.1 pipelining.
    dispatch(proxy, &HttpProxy::enqueue, response, *event.request);
  } else {
    VLOG(1) << "Returning '404 Not Found' for '" << event.request->path << "'";

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(event.socket);

    // Enqueue the response with the HttpProxy so that it respects the
    // order of requests to account for HTTP/1.1 pipelining.
    dispatch(proxy, &HttpProxy::enqueue, NotFound(), *event.request);
  }
}


void ProcessBase::visit(const ExitedEvent& event)
{
  exited(event.pid);
}


void ProcessBase::visit(const TerminateEvent& event)
{
  finalize();
}


UPID ProcessBase::link(const UPID& to)
{
  if (!to) {
    return to;
  }

  process_manager->link(this, to);

  return to;
}


void ProcessBase::route(
    const string& name,
    const Option<string>& help_,
    const HttpRequestHandler& handler)
{
  // Routes must start with '/'.
  CHECK(name.find('/') == 0);
  handlers.http[name.substr(1)] = handler;
  dispatch(help, &Help::add, pid.id, name, help_);
}



UPID spawn(ProcessBase* process, bool manage)
{
  process::initialize();

  if (process != NULL) {
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
  if (__process__ != NULL && __process__->self() == pid) {
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


void filter(Filter *filter)
{
  process::initialize();

  synchronized (filterer_mutex) {
    filterer = filter;
  }
}


void post(const UPID& to, const string& name, const char* data, size_t length)
{
  process::initialize();

  if (!to) {
    return;
  }

  // Encode and transport outgoing message.
  transport(encode(UPID(), to, name, string(data, length)));
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

  // Encode and transport outgoing message.
  transport(encode(from, to, name, string(data, length)));
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
