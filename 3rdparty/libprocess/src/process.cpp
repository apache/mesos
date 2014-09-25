#include <errno.h>
#include <ev.h>
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
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <vector>

#include <boost/shared_array.hpp>

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
#include <process/node.hpp>
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
#include <stout/memory.hpp> // TODO(benh): Replace shared_ptr with unique_ptr.
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/thread.hpp>
#include <stout/unreachable.hpp>

#include "config.hpp"
#include "decoder.hpp"
#include "encoder.hpp"
#include "gate.hpp"
#include "process_reference.hpp"
#include "synchronized.hpp"

using namespace process::metrics::internal;

using process::wait; // Necessary on some OS's to disambiguate.

using process::http::Accepted;
using process::http::BadRequest;
using process::http::InternalServerError;
using process::http::NotFound;
using process::http::OK;
using process::http::Request;
using process::http::Response;
using process::http::ServiceUnavailable;

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
  static synchronizable(prefixes) = SYNCHRONIZED_INITIALIZER;

  int id;
  synchronized (prefixes) {
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

  // Handles stream (i.e., pipe) based responses.
  void stream(const Future<short>& poll, const Request& request);

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

    // Helper for cleaning up a response (i.e., closing any open pipes
    // in the event Response::type is PIPE).
    static void cleanup(const Response& response)
    {
      if (response.type == Response::PIPE) {
        os::close(response.pipe);
      }
    }

    const Request request; // Make a copy.
    Future<Response>* future;
  };

  queue<Item*> items;

  Option<int> pipe; // Current pipe, if streaming.
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

  Socket accepted(int s);

  void link(ProcessBase* process, const UPID& to);

  PID<HttpProxy> proxy(const Socket& socket);

  void send(Encoder* encoder, bool persist);
  void send(const Response& response,
            const Request& request,
            const Socket& socket);
  void send(Message* message);

  Encoder* next(int s);

  void close(int s);

  void exited(const Node& node);
  void exited(ProcessBase* process);

private:
  // Map from UPID (local/remote) to process.
  map<UPID, set<ProcessBase*> > links;

  // Collection of all actice sockets.
  map<int, Socket> sockets;

  // Collection of sockets that should be disposed when they are
  // finished being used (e.g., when there is no more data to send on
  // them).
  set<int> dispose;

  // Map from socket to node (ip, port).
  map<int, Node> nodes;

  // Maps from node (ip, port) to temporary sockets (i.e., they will
  // get closed once there is no more data to send on them).
  map<Node, int> temps;

  // Maps from node (ip, port) to persistent sockets (i.e., they will
  // remain open even if there is no more data to send on them).  We
  // distinguish these from the 'temps' collection so we can tell when
  // a persistant socket has been lost (and thus generate
  // ExitedEvents).
  map<Node, int> persists;

  // Map from socket to outgoing queue.
  map<int, queue<Encoder*> > outgoing;

  // HTTP proxies.
  map<int, HttpProxy*> proxies;

  // Protects instance variables.
  synchronizable(this);
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
  synchronizable(processes);

  // Gates for waiting threads (protected by synchronizable(processes)).
  map<ProcessBase*, Gate*> gates;

  // Queue of runnable processes (implemented using list).
  list<ProcessBase*> runq;
  synchronizable(runq);

  // Number of running processes, to support Clock::settle operation.
  int running;
};


// Help strings.
const string Logging::TOGGLE_HELP = HELP(
    TLDR(
        "Sets the logging verbosity level for a specified duration."),
    USAGE(
        "/logging/toggle?level=VALUE&duration=VALUE"),
    DESCRIPTION(
        "The libprocess library uses [glog][glog] for logging. The library",
        "only uses verbose logging which means nothing will be output unless",
        "the verbosity level is set (by default it's 0, libprocess uses"
        "levels 1, 2, and 3).",
        "",
        "**NOTE:** If your application uses glog this will also affect",
        "your verbose logging.",
        "",
        "Required query parameters:",
        "",
        ">        level=VALUE          Verbosity level (e.g., 1, 2, 3)",
        ">        duration=VALUE       Duration to keep verbosity level",
        ">                             toggled (e.g., 10secs, 15mins, etc.)"),
    REFERENCES(
        "[glog]: https://code.google.com/p/google-glog"));


const string Profiler::START_HELP = HELP(
    TLDR(
        "Starts profiling ..."),
    USAGE(
        "/profiler/start..."),
    DESCRIPTION(
        "...",
        "",
        "Query parameters:",
        "",
        ">        param=VALUE          Some description here"));


const string Profiler::STOP_HELP = HELP(
    TLDR(
        "Stops profiling ..."),
    USAGE(
        "/profiler/stop..."),
    DESCRIPTION(
        "...",
        "",
        "Query parameters:",
        "",
        ">        param=VALUE          Some description here"));


// Unique id that can be assigned to each process.
static uint32_t __id__ = 0;

// Local server socket.
static int __s__ = -1;

// Local IP address.
static uint32_t __ip__ = 0;

// Local port.
static uint16_t __port__ = 0;

// Active SocketManager (eventually will probably be thread-local).
static SocketManager* socket_manager = NULL;

// Active ProcessManager (eventually will probably be thread-local).
static ProcessManager* process_manager = NULL;

// Event loop.
static struct ev_loop* loop = NULL;

// Asynchronous watcher for interrupting loop.
static ev_async async_watcher;

// Watcher for timeouts.
static ev_timer timeouts_watcher;

// Server watcher for accepting connections.
static ev_io server_watcher;

// Queue of I/O watchers to be asynchronously added to the event loop
// (protected by 'watchers' below).
// TODO(benh): Replace this queue with functions that we put in
// 'functions' below that perform the ev_io_start themselves.
static queue<ev_io*>* watchers = new queue<ev_io*>();
static synchronizable(watchers) = SYNCHRONIZED_INITIALIZER;

// Queue of functions to be invoked asynchronously within the vent
// loop (protected by 'watchers' below).
static queue<lambda::function<void(void)> >* functions =
  new queue<lambda::function<void(void)> >();

// We store the timers in a map of lists indexed by the timeout of the
// timer so that we can have two timers that have the same timeout. We
// exploit that the map is SORTED!
static map<Time, list<Timer> >* timeouts = new map<Time, list<Timer> >();
static synchronizable(timeouts) = SYNCHRONIZED_INITIALIZER_RECURSIVE;

// For supporting Clock::settle(), true if timers have been removed
// from 'timeouts' but may not have been executed yet. Protected by
// the timeouts lock. This is only used when the clock is paused.
static bool pending_timers = false;

// Flag to indicate whether or to update the timer on async interrupt.
static bool update_timer = false;

// Scheduling gate that threads wait at when there is nothing to run.
static Gate* gate = new Gate();

// Filter. Synchronized support for using the filterer needs to be
// recursive incase a filterer wants to do anything fancy (which is
// possible and likely given that filters will get used for testing).
static Filter* filterer = NULL;
static synchronizable(filterer) = SYNCHRONIZED_INITIALIZER_RECURSIVE;

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


// We namespace the clock related variables to keep them well
// named. In the future we'll probably want to associate a clock with
// a specific ProcessManager/SocketManager instance pair, so this will
// likely change.
namespace clock {

map<ProcessBase*, Time>* currents = new map<ProcessBase*, Time>();

// TODO(dhamon): These static non-POD instances should be replaced by pointers
// or functions.
Time initial = Time::epoch();
Time current = Time::epoch();

Duration advanced = Duration::zero();

bool paused = false;

} // namespace clock {


Time Clock::now()
{
  return now(__process__);
}


Time Clock::now(ProcessBase* process)
{
  synchronized (timeouts) {
    if (Clock::paused()) {
      if (process != NULL) {
        if (clock::currents->count(process) != 0) {
          return (*clock::currents)[process];
        } else {
          return (*clock::currents)[process] = clock::initial;
        }
      } else {
        return clock::current;
      }
    }
  }

  // TODO(benh): Versus ev_now()?
  double d = ev_time();
  Try<Time> time = Time::create(d); // Compensates for clock::advanced.

  // TODO(xujyan): Move CHECK_SOME to libprocess and add CHECK_SOME
  // here.
  if (time.isError()) {
    LOG(FATAL) << "Failed to create a Time from " << d << ": "
               << time.error();
  }
  return time.get();
}


void Clock::pause()
{
  process::initialize(); // To make sure the libev watchers are ready.

  synchronized (timeouts) {
    if (!clock::paused) {
      clock::initial = clock::current = now();
      clock::paused = true;
      VLOG(2) << "Clock paused at " << clock::initial;
    }
  }

  // Note that after pausing the clock an existing libev timer might
  // still fire (invoking handle_timeout), but since paused == true no
  // "time" will actually have passed, so no timer will actually fire.
}


bool Clock::paused()
{
  return clock::paused;
}


void Clock::resume()
{
  process::initialize(); // To make sure the libev watchers are ready.

  synchronized (timeouts) {
    if (clock::paused) {
      VLOG(2) << "Clock resumed at " << clock::current;
      clock::paused = false;
      clock::currents->clear();
      update_timer = true;
      ev_async_send(loop, &async_watcher);
    }
  }
}


void Clock::advance(const Duration& duration)
{
  synchronized (timeouts) {
    if (clock::paused) {
      clock::advanced += duration;
      clock::current += duration;
      VLOG(2) << "Clock advanced ("  << duration << ") to " << clock::current;
      if (!update_timer) {
        update_timer = true;
        ev_async_send(loop, &async_watcher);
      }
    }
  }
}


void Clock::advance(ProcessBase* process, const Duration& duration)
{
  synchronized (timeouts) {
    if (clock::paused) {
      Time current = now(process);
      current += duration;
      (*clock::currents)[process] = current;
      VLOG(2) << "Clock of " << process->self() << " advanced (" << duration
              << ") to " << current;
    }
  }
}


void Clock::update(const Time& time)
{
  synchronized (timeouts) {
    if (clock::paused) {
      if (clock::current < time) {
        clock::advanced += (time - clock::current);
        clock::current = Time(time);
        VLOG(2) << "Clock updated to " << clock::current;
        if (!update_timer) {
          update_timer = true;
          ev_async_send(loop, &async_watcher);
        }
      }
    }
  }
}


void Clock::update(ProcessBase* process, const Time& time)
{
  synchronized (timeouts) {
    if (clock::paused) {
      if (now(process) < time) {
        VLOG(2) << "Clock of " << process->self() << " updated to " << time;
        (*clock::currents)[process] = Time(time);
      }
    }
  }
}


void Clock::order(ProcessBase* from, ProcessBase* to)
{
  update(to, now(from));
}


void Clock::settle()
{
  CHECK(clock::paused); // TODO(benh): Consider returning a bool instead.
  process_manager->settle();
}


Try<Time> Time::create(double seconds)
{
  Try<Duration> duration = Duration::create(seconds);
  if (duration.isSome()) {
    // In production code, clock::advanced will always be zero!
    return Time(duration.get() + clock::advanced);
  } else {
    return Error("Argument too large for Time: " + duration.error());
  }
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
  if (message->to.ip == __ip__ && message->to.port == __port__) {
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
    const string& identifier = "libprocess/";
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

  const UPID to(decode.get(), __ip__, __port__);

  // And now determine 'name'.
  index = index != string::npos ? index + 2: request->path.size();
  const string& name = request->path.substr(index);

  VLOG(2) << "Parsed message name '" << name
          << "' for " << to << " from " << from.get();

  Message* message = new Message();
  message->name = name;
  message->from = from.get();
  message->to = to;
  message->body = request->body;

  return message;
}

// Wrapper around function we want to run in the event loop.
template <typename T>
void _run_in_event_loop(
    const lambda::function<Future<T>(void)>& f,
    const Owned<Promise<T> >& promise)
{
  // Don't bother running the function if the future has been discarded.
  if (promise->future().hasDiscard()) {
    promise->discard();
  } else {
    promise->set(f());
  }
}


// Helper for running a function in the event loop.
template <typename T>
Future<T> run_in_event_loop(const lambda::function<Future<T>(void)>& f)
{
  Owned<Promise<T> > promise(new Promise<T>());

  Future<T> future = promise->future();

  // Enqueue the function.
  synchronized (watchers) {
    functions->push(lambda::bind(&_run_in_event_loop<T>, f, promise));
  }

  // Interrupt the loop.
  ev_async_send(loop, &async_watcher);

  return future;
}


void handle_async(struct ev_loop* loop, ev_async* _, int revents)
{
  synchronized (watchers) {
    // Start all the new I/O watchers.
    while (!watchers->empty()) {
      ev_io* watcher = watchers->front();
      watchers->pop();
      ev_io_start(loop, watcher);
    }

    while (!functions->empty()) {
      (functions->front())();
      functions->pop();
    }
  }

  synchronized (timeouts) {
    if (update_timer) {
      if (!timeouts->empty()) {
        // Determine when the next timer should fire.
        timeouts_watcher.repeat =
          (timeouts->begin()->first - Clock::now()).secs();

        if (timeouts_watcher.repeat <= 0) {
          // Feed the event now!
          timeouts_watcher.repeat = 0;
          ev_timer_again(loop, &timeouts_watcher);
          ev_feed_event(loop, &timeouts_watcher, EV_TIMEOUT);
        } else {
          // Don't fire the timer if the clock is paused since we
          // don't want time to advance (instead a call to
          // clock::advance() will handle the timer).
          if (Clock::paused() && timeouts_watcher.repeat > 0) {
            timeouts_watcher.repeat = 0;
          }

          ev_timer_again(loop, &timeouts_watcher);
        }
      }

      update_timer = false;
    }
  }
}


void handle_timeouts(struct ev_loop* loop, ev_timer* _, int revents)
{
  list<Timer> timedout;

  synchronized (timeouts) {
    Time now = Clock::now();

    VLOG(3) << "Handling timeouts up to " << now;

    foreachkey (const Time& timeout, *timeouts) {
      if (timeout > now) {
        break;
      }

      VLOG(3) << "Have timeout(s) at " << timeout;

      // Record that we have pending timers to execute so the
      // Clock::settle() operation can wait until we're done.
      pending_timers = true;

      foreach (const Timer& timer, (*timeouts)[timeout]) {
        timedout.push_back(timer);
      }
    }

    // Now erase the range of timeouts that timed out.
    timeouts->erase(timeouts->begin(), timeouts->upper_bound(now));

    // Okay, so the timeout for the next timer should not have fired.
    CHECK(timeouts->empty() || (timeouts->begin()->first > now));

    // Update the timer as necessary.
    if (!timeouts->empty()) {
      // Determine when the next timer should fire.
      timeouts_watcher.repeat =
        (timeouts->begin()->first - Clock::now()).secs();

      if (timeouts_watcher.repeat <= 0) {
        // Feed the event now!
        timeouts_watcher.repeat = 0;
        ev_timer_again(loop, &timeouts_watcher);
        ev_feed_event(loop, &timeouts_watcher, EV_TIMEOUT);
      } else {
        // Don't fire the timer if the clock is paused since we don't
        // want time to advance (instead a call to Clock::advance()
        // will handle the timer).
        if (Clock::paused() && timeouts_watcher.repeat > 0) {
          timeouts_watcher.repeat = 0;
        }

        ev_timer_again(loop, &timeouts_watcher);
      }
    }

    update_timer = false; // Since we might have a queued update_timer.
  }

  // Update current time of process (if it's present/valid). It might
  // be necessary to actually add some more synchronization around
  // this so that, for example, pausing and resuming the clock doesn't
  // cause some processes to get thier current times updated and
  // others not. Since ProcessManager::use acquires the 'processes'
  // lock we had to move this out of the synchronized (timeouts) above
  // since there was a deadlock with acquring 'processes' then
  // 'timeouts' (reverse order) in ProcessManager::cleanup. Note that
  // current time may be greater than the timeout if a local message
  // was received (and happens-before kicks in).
  if (Clock::paused()) {
    foreach (const Timer& timer, timedout) {
      if (ProcessReference process = process_manager->use(timer.creator())) {
        Clock::update(process, timer.timeout().time());
      }
    }
  }

  // Invoke the timers that timed out (TODO(benh): Do this
  // asynchronously so that we don't tie up the event thread!).
  foreach (const Timer& timer, timedout) {
    timer();
  }

  // Mark ourselves as done executing the timers since it's now safe
  // for a call to Clock::settle() to check if there will be any
  // future timeouts reached.
  synchronized (timeouts) {
    pending_timers = false;
  }
}


void recv_data(struct ev_loop* loop, ev_io* watcher, int revents)
{
  DataDecoder* decoder = (DataDecoder*) watcher->data;

  int s = watcher->fd;

  while (true) {
    const ssize_t size = 80 * 1024;
    ssize_t length = 0;

    char data[size];

    length = recv(s, data, size, 0);

    if (length < 0 && (errno == EINTR)) {
      // Interrupted, try again now.
      continue;
    } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Might block, try again later.
      break;
    } else if (length <= 0) {
      // Socket error or closed.
      if (length < 0) {
        const char* error = strerror(errno);
        VLOG(1) << "Socket error while receiving: " << error;
      } else {
        VLOG(2) << "Socket closed while receiving";
      }
      socket_manager->close(s);
      delete decoder;
      ev_io_stop(loop, watcher);
      delete watcher;
      break;
    } else {
      CHECK(length > 0);

      // Decode as much of the data as possible into HTTP requests.
      const deque<Request*>& requests = decoder->decode(data, length);

      if (!requests.empty()) {
        foreach (Request* request, requests) {
          process_manager->handle(decoder->socket(), request);
        }
      } else if (requests.empty() && decoder->failed()) {
        VLOG(1) << "Decoder error while receiving";
        socket_manager->close(s);
        delete decoder;
        ev_io_stop(loop, watcher);
        delete watcher;
        break;
      }
    }
  }
}


// A variant of 'recv_data' that doesn't do anything with the
// data. Used by sockets created via SocketManager::link as well as
// SocketManager::send(Message) where we don't care about the data
// received we mostly just want to know when the socket has been
// closed.
void ignore_data(struct ev_loop* loop, ev_io* watcher, int revents)
{
  Socket* socket = (Socket*) watcher->data;

  int s = watcher->fd;

  while (true) {
    const ssize_t size = 80 * 1024;
    ssize_t length = 0;

    char data[size];

    length = recv(s, data, size, 0);

    if (length < 0 && (errno == EINTR)) {
      // Interrupted, try again now.
      continue;
    } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Might block, try again later.
      break;
    } else if (length <= 0) {
      // Socket error or closed.
      if (length < 0) {
        const char* error = strerror(errno);
        VLOG(1) << "Socket error while receiving: " << error;
      } else {
        VLOG(2) << "Socket closed while receiving";
      }
      socket_manager->close(s);
      ev_io_stop(loop, watcher);
      delete socket;
      delete watcher;
      break;
    } else {
      VLOG(2) << "Ignoring " << length << " bytes of data received "
              << "on socket used only for sending";
    }
  }
}


void send_data(struct ev_loop* loop, ev_io* watcher, int revents)
{
  DataEncoder* encoder = (DataEncoder*) watcher->data;

  int s = watcher->fd;

  while (true) {
    const void* data;
    size_t size;

    data = encoder->next(&size);
    CHECK(size > 0);

    ssize_t length = send(s, data, size, MSG_NOSIGNAL);

    if (length < 0 && (errno == EINTR)) {
      // Interrupted, try again now.
      encoder->backup(size);
      continue;
    } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Might block, try again later.
      encoder->backup(size);
      break;
    } else if (length <= 0) {
      // Socket error or closed.
      if (length < 0) {
        const char* error = strerror(errno);
        VLOG(1) << "Socket error while sending: " << error;
      } else {
        VLOG(1) << "Socket closed while sending";
      }
      socket_manager->close(s);
      delete encoder;
      ev_io_stop(loop, watcher);
      delete watcher;
      break;
    } else {
      CHECK(length > 0);

      // Update the encoder with the amount sent.
      encoder->backup(size - length);

      // See if there is any more of the message to send.
      if (encoder->remaining() == 0) {
        delete encoder;

        // Stop this watcher for now.
        ev_io_stop(loop, watcher);

        // Check for more stuff to send on socket.
        Encoder* next = socket_manager->next(s);
        if (next != NULL) {
          watcher->data = next;
          ev_io_init(watcher, next->sender(), s, EV_WRITE);
          ev_io_start(loop, watcher);
        } else {
          // Nothing more to send right now, clean up.
          delete watcher;
        }
        break;
      }
    }
  }
}


void send_file(struct ev_loop* loop, ev_io* watcher, int revents)
{
  FileEncoder* encoder = (FileEncoder*) watcher->data;

  int s = watcher->fd;

  while (true) {
    int fd;
    off_t offset;
    size_t size;

    fd = encoder->next(&offset, &size);
    CHECK(size > 0);

    ssize_t length = os::sendfile(s, fd, offset, size);

    if (length < 0 && (errno == EINTR)) {
      // Interrupted, try again now.
      encoder->backup(size);
      continue;
    } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Might block, try again later.
      encoder->backup(size);
      break;
    } else if (length <= 0) {
      // Socket error or closed.
      if (length < 0) {
        const char* error = strerror(errno);
        VLOG(1) << "Socket error while sending: " << error;
      } else {
        VLOG(1) << "Socket closed while sending";
      }
      socket_manager->close(s);
      delete encoder;
      ev_io_stop(loop, watcher);
      delete watcher;
      break;
    } else {
      CHECK(length > 0);

      // Update the encoder with the amount sent.
      encoder->backup(size - length);

      // See if there is any more of the message to send.
      if (encoder->remaining() == 0) {
        delete encoder;

        // Stop this watcher for now.
        ev_io_stop(loop, watcher);

        // Check for more stuff to send on socket.
        Encoder* next = socket_manager->next(s);
        if (next != NULL) {
          watcher->data = next;
          ev_io_init(watcher, next->sender(), s, EV_WRITE);
          ev_io_start(loop, watcher);
        } else {
          // Nothing more to send right now, clean up.
          delete watcher;
        }
        break;
      }
    }
  }
}


void sending_connect(struct ev_loop* loop, ev_io* watcher, int revents)
{
  int s = watcher->fd;

  // Now check that a successful connection was made.
  int opt;
  socklen_t optlen = sizeof(opt);

  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0 || opt != 0) {
    // Connect failure.
    VLOG(1) << "Socket error while connecting";
    socket_manager->close(s);
    MessageEncoder* encoder = (MessageEncoder*) watcher->data;
    delete encoder;
    ev_io_stop(loop, watcher);
    delete watcher;
  } else {
    // We're connected! Now let's do some sending.
    ev_io_stop(loop, watcher);
    ev_io_init(watcher, send_data, s, EV_WRITE);
    ev_io_start(loop, watcher);
  }
}


void receiving_connect(struct ev_loop* loop, ev_io* watcher, int revents)
{
  int s = watcher->fd;

  // Now check that a successful connection was made.
  int opt;
  socklen_t optlen = sizeof(opt);

  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0 || opt != 0) {
    // Connect failure.
    VLOG(1) << "Socket error while connecting";
    socket_manager->close(s);
    Socket* socket = (Socket*) watcher->data;
    delete socket;
    ev_io_stop(loop, watcher);
    delete watcher;
  } else {
    // We're connected! Now let's do some receiving.
    ev_io_stop(loop, watcher);
    ev_io_init(watcher, ignore_data, s, EV_READ);
    ev_io_start(loop, watcher);
  }
}


void accept(struct ev_loop* loop, ev_io* watcher, int revents)
{
  CHECK_EQ(__s__, watcher->fd);

  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  int s = ::accept(__s__, (sockaddr*) &addr, &addrlen);

  if (s < 0) {
    return;
  }

  Try<Nothing> nonblock = os::nonblock(s);
  if (nonblock.isError()) {
    LOG_IF(INFO, VLOG_IS_ON(1)) << "Failed to accept, nonblock: "
                                << nonblock.error();
    os::close(s);
    return;
  }

  Try<Nothing> cloexec = os::cloexec(s);
  if (cloexec.isError()) {
    LOG_IF(INFO, VLOG_IS_ON(1)) << "Failed to accept, cloexec: "
                                << cloexec.error();
    os::close(s);
    return;
  }

  // Turn off Nagle (TCP_NODELAY) so pipelined requests don't wait.
  int on = 1;
  if (setsockopt(s, SOL_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    const char* error = strerror(errno);
    VLOG(1) << "Failed to turn off the Nagle algorithm: " << error;
    os::close(s);
  } else {
    // Inform the socket manager for proper bookkeeping.
    const Socket& socket = socket_manager->accepted(s);

    // Allocate and initialize the decoder and watcher.
    DataDecoder* decoder = new DataDecoder(socket);

    ev_io* watcher = new ev_io();
    watcher->data = decoder;

    ev_io_init(watcher, recv_data, s, EV_READ);
    ev_io_start(loop, watcher);
  }
}


// Data necessary for polling so we can discard polling and actually
// stop it in the event loop.
struct Poll
{
  Poll()
  {
    // Need to explicitly instantiate the watchers.
    watcher.io.reset(new ev_io());
    watcher.async.reset(new ev_async());
  }

  // An I/O watcher for checking for readability or writeability and
  // an async watcher for being able to discard the polling.
  struct {
    memory::shared_ptr<ev_io> io;
    memory::shared_ptr<ev_async> async;
  } watcher;

  Promise<short> promise;
};


// Event loop callback when I/O is ready on polling file descriptor.
void polled(struct ev_loop* loop, ev_io* watcher, int revents)
{
  Poll* poll = (Poll*) watcher->data;

  ev_io_stop(loop, poll->watcher.io.get());

  // Stop the async watcher (also clears if pending so 'discard_poll'
  // will not get invoked and we can delete 'poll' here).
  ev_async_stop(loop, poll->watcher.async.get());

  poll->promise.set(revents);

  delete poll;
}


// Event loop callback when future associated with polling file
// descriptor has been discarded.
void discard_poll(struct ev_loop* loop, ev_async* watcher, int revents)
{
  Poll* poll = (Poll*) watcher->data;

  // Check and see if we have a pending 'polled' callback and if so
  // let it "win".
  if (ev_is_pending(poll->watcher.io.get())) {
    return;
  }

  ev_async_stop(loop, poll->watcher.async.get());

  // Stop the I/O watcher (but note we check if pending above) so it
  // won't get invoked and we can delete 'poll' here.
  ev_io_stop(loop, poll->watcher.io.get());

  poll->promise.discard();

  delete poll;
}


void* serve(void* arg)
{
  ev_loop(((struct ev_loop*) arg), 0);

  return NULL;
}


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

  __ip__ = 0;
  __port__ = 0;

  char* value;

  // Check environment for ip.
  value = getenv("LIBPROCESS_IP");
  if (value != NULL) {
    int result = inet_pton(AF_INET, value, &__ip__);
    if (result == 0) {
      LOG(FATAL) << "LIBPROCESS_IP=" << value << " was unparseable";
    } else if (result < 0) {
      PLOG(FATAL) << "Failed to initialize, inet_pton";
    }
  }

  // Check environment for port.
  value = getenv("LIBPROCESS_PORT");
  if (value != NULL) {
    int result = atoi(value);
    if (result < 0 || result > USHRT_MAX) {
      LOG(FATAL) << "LIBPROCESS_PORT=" << value << " is not a valid port";
    }
    __port__ = result;
  }

  // Create a "server" socket for communicating with other nodes.
  if ((__s__ = ::socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    PLOG(FATAL) << "Failed to initialize, socket";
  }

  // Make socket non-blocking.
  Try<Nothing> nonblock = os::nonblock(__s__);
  if (nonblock.isError()) {
    LOG(FATAL) << "Failed to initialize, nonblock: " << nonblock.error();
  }

  // Set FD_CLOEXEC flag.
  Try<Nothing> cloexec = os::cloexec(__s__);
  if (cloexec.isError()) {
    LOG(FATAL) << "Failed to initialize, cloexec: " << cloexec.error();
  }

  // Allow address reuse.
  int on = 1;
  if (setsockopt(__s__, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    PLOG(FATAL) << "Failed to initialize, setsockopt(SO_REUSEADDR)";
  }

  // Set up socket.
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = PF_INET;
  addr.sin_addr.s_addr = __ip__;
  addr.sin_port = htons(__port__);

  if (bind(__s__, (sockaddr*) &addr, sizeof(addr)) < 0) {
    PLOG(FATAL) << "Failed to initialize, bind "
                << inet_ntoa(addr.sin_addr) << ":" << __port__;
  }

  // Lookup and store assigned ip and assigned port.
  socklen_t addrlen = sizeof(addr);
  if (getsockname(__s__, (sockaddr*) &addr, &addrlen) < 0) {
    PLOG(FATAL) << "Failed to initialize, getsockname";
  }

  __ip__ = addr.sin_addr.s_addr;
  __port__ = ntohs(addr.sin_port);

  // Lookup hostname if missing ip or if ip is 127.0.0.1 in case we
  // actually have a valid external ip address. Note that we need only
  // one ip address, so that other processes can send and receive and
  // don't get confused as to whom they are sending to.
  if (__ip__ == 0 || __ip__ == 2130706433) {
    char hostname[512];

    if (gethostname(hostname, sizeof(hostname)) < 0) {
      LOG(FATAL) << "Failed to initialize, gethostname: "
                 << hstrerror(h_errno);
    }

    // Lookup IP address of local hostname.
    hostent* he;

    if ((he = gethostbyname2(hostname, AF_INET)) == NULL) {
      LOG(FATAL) << "Failed to initialize, gethostbyname2: "
                 << hstrerror(h_errno);
    }

    __ip__ = *((uint32_t *) he->h_addr_list[0]);
  }

  if (listen(__s__, 500000) < 0) {
    PLOG(FATAL) << "Failed to initialize, listen";
  }

  // Setup event loop.
#ifdef __sun__
  loop = ev_default_loop(EVBACKEND_POLL | EVBACKEND_SELECT);
#else
  loop = ev_default_loop(EVFLAG_AUTO);
#endif // __sun__

  ev_async_init(&async_watcher, handle_async);
  ev_async_start(loop, &async_watcher);

  ev_timer_init(&timeouts_watcher, handle_timeouts, 0., 2100000.0);
  ev_timer_again(loop, &timeouts_watcher);

  ev_io_init(&server_watcher, accept, __s__, EV_READ);
  ev_io_start(loop, &server_watcher);

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
  if (pthread_create(&thread, NULL, serve, loop) != 0) {
    LOG(FATAL) << "Failed to initialize, pthread_create";
  }

  // Need to set initialzing here so that we can actually invoke
  // 'spawn' below for the garbage collector.
  initializing = false;

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
  // value = getenv("LIBPROCESS_STATISTICS_WINDOW");
  // if (value != NULL) {
  //   Try<Duration> window = Duration::parse(string(value));
  //   if (window.isError()) {
  //     LOG(FATAL) << "LIBPROCESS_STATISTICS_WINDOW=" << value
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

  char temp[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, (in_addr*) &__ip__, temp, INET_ADDRSTRLEN) == NULL) {
    PLOG(FATAL) << "Failed to initialize, inet_ntop";
  }

  VLOG(1) << "libprocess is initialized on " << temp << ":" << __port__
          << " for " << cpus << " cpus";
}


uint32_t ip()
{
  process::initialize();
  return __ip__;
}


uint16_t port()
{
  process::initialize();
  return __port__;
}


HttpProxy::HttpProxy(const Socket& _socket)
  : ProcessBase(ID::generate("__http__")),
    socket(_socket) {}


HttpProxy::~HttpProxy()
{
  // Need to make sure response producers know not to continue to
  // create a response (streaming or otherwise).
  if (pipe.isSome()) {
    os::close(pipe.get());
  }
  pipe = None();

  while (!items.empty()) {
    Item* item = items.front();

    // Attempt to discard the future.
    item->future->discard();

    // But it might have already been ready. In general, we need to
    // wait until this future is potentially ready in order to attempt
    // to close a pipe if one exists.
    item->future->onReady(lambda::bind(&Item::cleanup, lambda::_1));

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

    // Make sure the pipe is nonblocking.
    Try<Nothing> nonblock = os::nonblock(response.pipe);
    if (nonblock.isError()) {
      const char* error = strerror(errno);
      VLOG(1) << "Failed make pipe nonblocking: " << error;
      socket_manager->send(InternalServerError(), request, socket);
      return true; // All done, can process next response.
    }

    // While the user is expected to properly set a 'Content-Type'
    // header, we fill in (or overwrite) 'Transfer-Encoding' header.
    response.headers["Transfer-Encoding"] = "chunked";

    VLOG(1) << "Starting \"chunked\" streaming";

    socket_manager->send(
        new HttpResponseEncoder(socket, response, request),
        true);

    pipe = response.pipe;

    io::poll(pipe.get(), io::READ).onAny(
        defer(self(), &Self::stream, lambda::_1, request));

    return false; // Streaming, don't process next response (yet)!
  } else {
    socket_manager->send(response, request, socket);
  }

  return true; // All done, can process next response.
}


void HttpProxy::stream(const Future<short>& poll, const Request& request)
{
  // TODO(benh): Use 'splice' on Linux.

  CHECK(pipe.isSome());

  bool finished = false; // Whether we're done streaming.

  if (poll.isReady()) {
    // Read and write.
    CHECK(poll.get() == io::READ);
    const size_t size = 4 * 1024; // 4K.
    char data[size];
    while (!finished) {
      ssize_t length = ::read(pipe.get(), data, size);
      if (length < 0 && (errno == EINTR)) {
        // Interrupted, try again now.
        continue;
      } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Might block, try again later.
        io::poll(pipe.get(), io::READ).onAny(
            defer(self(), &Self::stream, lambda::_1, request));
        break;
      } else {
        std::ostringstream out;
        if (length <= 0) {
          // Error or closed, treat both as closed.
          if (length < 0) {
            // Error.
            const char* error = strerror(errno);
            VLOG(1) << "Read error while streaming: " << error;
          }
          out << "0\r\n" << "\r\n";
          finished = true;
        } else {
          // Data!
          out << std::hex << length << "\r\n";
          out.write(data, length);
          out << "\r\n";
        }

        // We always persist the connection when we're not finished
        // streaming.
        socket_manager->send(
            new DataEncoder(socket, out.str()),
            finished ? request.keepAlive : true);
      }
    }
  } else if (poll.isFailed()) {
    VLOG(1) << "Failed to poll: " << poll.failure();
    socket_manager->send(InternalServerError(), request, socket);
    finished = true;
  } else {
    VLOG(1) << "Unexpected discarded future while polling";
    socket_manager->send(InternalServerError(), request, socket);
    finished = true;
  }

  if (finished) {
    os::close(pipe.get());
    pipe = None();
    next();
  }
}


SocketManager::SocketManager()
{
  synchronizer(this) = SYNCHRONIZED_INITIALIZER_RECURSIVE;
}


SocketManager::~SocketManager() {}


Socket SocketManager::accepted(int s)
{
  synchronized (this) {
    return sockets[s] = Socket(s);
  }

  return UNREACHABLE(); // Quiet the compiler.
}


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

  Node node(to.ip, to.port);

  synchronized (this) {
    // Check if node is remote and there isn't a persistant link.
    if ((node.ip != __ip__ || node.port != __port__)
        && persists.count(node) == 0) {
      // Okay, no link, let's create a socket.
      Try<int> socket = process::socket(AF_INET, SOCK_STREAM, 0);
      if (socket.isError()) {
        LOG(FATAL) << "Failed to link, socket: " << socket.error();
      }

      int s = socket.get();

      Try<Nothing> nonblock = os::nonblock(s);
      if (nonblock.isError()) {
        LOG(FATAL) << "Failed to link, nonblock: " << nonblock.error();
      }

      Try<Nothing> cloexec = os::cloexec(s);
      if (cloexec.isError()) {
        LOG(FATAL) << "Failed to link, cloexec: " << cloexec.error();
      }

      sockets[s] = Socket(s);
      nodes[s] = node;

      persists[node] = s;

      // Allocate and initialize a watcher for reading data from this
      // socket. Note that we don't expect to receive anything other
      // than HTTP '202 Accepted' responses which we anyway ignore.
      // We do, however, want to react when it gets closed so we can
      // generate appropriate lost events (since this is a 'link').
      ev_io* watcher = new ev_io();
      watcher->data = new Socket(sockets[s]);

      // Try and connect to the node using this socket.
      sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = PF_INET;
      addr.sin_port = htons(to.port);
      addr.sin_addr.s_addr = to.ip;

      if (connect(s, (sockaddr*) &addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
          PLOG(FATAL) << "Failed to link, connect";
        }

        // Wait for socket to be connected.
        ev_io_init(watcher, receiving_connect, s, EV_WRITE);
      } else {
        ev_io_init(watcher, ignore_data, s, EV_READ);
      }

      // Enqueue the watcher.
      synchronized (watchers) {
        watchers->push(watcher);
      }

      // Interrupt the loop.
      ev_async_send(loop, &async_watcher);
    }

    links[to].insert(process);
  }
}


PID<HttpProxy> SocketManager::proxy(const Socket& socket)
{
  HttpProxy* proxy = NULL;

  synchronized (this) {
    // This socket might have been asked to get closed (e.g., remote
    // side hang up) while a process is attempting to handle an HTTP
    // request. Thus, if there is no more socket, return an empty PID.
    if (sockets.count(socket) > 0) {
      if (proxies.count(socket) > 0) {
        return proxies[socket]->self();
      } else {
        proxy = new HttpProxy(sockets[socket]);
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


void SocketManager::send(Encoder* encoder, bool persist)
{
  CHECK(encoder != NULL);

  synchronized (this) {
    if (sockets.count(encoder->socket()) > 0) {
      // Update whether or not this socket should get disposed after
      // there is no more data to send.
      if (!persist) {
        dispose.insert(encoder->socket());
      }

      if (outgoing.count(encoder->socket()) > 0) {
        outgoing[encoder->socket()].push(encoder);
      } else {
        // Initialize the outgoing queue.
        outgoing[encoder->socket()];

        // Allocate and initialize the watcher.
        ev_io* watcher = new ev_io();
        watcher->data = encoder;

        ev_io_init(watcher, encoder->sender(), encoder->socket(), EV_WRITE);

        synchronized (watchers) {
          watchers->push(watcher);
        }

        ev_async_send(loop, &async_watcher);
      }
    } else {
      VLOG(1) << "Attempting to send on a no longer valid socket!";
      delete encoder;
    }
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


void SocketManager::send(Message* message)
{
  CHECK(message != NULL);

  Node node(message->to.ip, message->to.port);

  synchronized (this) {
    // Check if there is already a socket.
    bool persist = persists.count(node) > 0;
    bool temp = temps.count(node) > 0;
    if (persist || temp) {
      int s = persist ? persists[node] : temps[node];
      CHECK(sockets.count(s) > 0);
      send(new MessageEncoder(sockets[s], message), persist);
    } else {
      // No peristent or temporary socket to the node currently
      // exists, so we create a temporary one.
      Try<int> socket = process::socket(AF_INET, SOCK_STREAM, 0);
      if (socket.isError()) {
        LOG(FATAL) << "Failed to send, socket: " << socket.error();
      }

      int s = socket.get();

      Try<Nothing> nonblock = os::nonblock(s);
      if (nonblock.isError()) {
        LOG(FATAL) << "Failed to send, nonblock: " << nonblock.error();
      }

      Try<Nothing> cloexec = os::cloexec(s);
      if (cloexec.isError()) {
        LOG(FATAL) << "Failed to send, cloexec: " << cloexec.error();
      }

      sockets[s] = Socket(s);
      nodes[s] = node;
      temps[node] = s;

      dispose.insert(s);

      // Initialize the outgoing queue.
      outgoing[s];

      // Allocate and initialize a watcher for reading data from this
      // socket. Note that we don't expect to receive anything other
      // than HTTP '202 Accepted' responses which we anyway ignore.
      ev_io* watcher = new ev_io();
      watcher->data = new Socket(sockets[s]);

      ev_io_init(watcher, ignore_data, s, EV_READ);

      // Enqueue the watcher.
      synchronized (watchers) {
        watchers->push(watcher);
      }

      // Allocate and initialize a watcher for sending the message.
      watcher = new ev_io();
      watcher->data = new MessageEncoder(sockets[s], message);

      // Try and connect to the node using this socket.
      sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = PF_INET;
      addr.sin_port = htons(message->to.port);
      addr.sin_addr.s_addr = message->to.ip;

      if (connect(s, (sockaddr*) &addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
          PLOG(FATAL) << "Failed to send, connect";
        }

        // Initialize watcher for connecting.
        ev_io_init(watcher, sending_connect, s, EV_WRITE);
      } else {
        // Initialize watcher for sending.
        ev_io_init(watcher, send_data, s, EV_WRITE);
      }

      // Enqueue the watcher.
      synchronized (watchers) {
        watchers->push(watcher);
      }

      ev_async_send(loop, &async_watcher);
    }
  }
}


Encoder* SocketManager::next(int s)
{
  HttpProxy* proxy = NULL; // Non-null if needs to be terminated.

  synchronized (this) {
    // We cannot assume 'sockets.count(s) > 0' here because it's
    // possible that 's' has been removed with a a call to
    // SocketManager::close. For example, it could be the case that a
    // socket has gone to CLOSE_WAIT and the call to 'recv' in
    // recv_data returned 0 causing SocketManager::close to get
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
          if (nodes.count(s) > 0) {
            const Node& node = nodes[s];
            CHECK(temps.count(node) > 0 && temps[node] == s);
            temps.erase(node);
            nodes.erase(s);
          }

          if (proxies.count(s) > 0) {
            proxy = proxies[s];
            proxies.erase(s);
          }

          dispose.erase(s);
          sockets.erase(s);

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

  synchronized (this) {
    // This socket might not be active if it was already asked to get
    // closed (e.g., a write on the socket failed so we try and close
    // it and then later the read side of the socket gets closed so we
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

      // Clean up after sockets used for node communication.
      if (nodes.count(s) > 0) {
        const Node& node = nodes[s];

        // Don't bother invoking exited unless socket was persistant.
        if (persists.count(node) > 0 && persists[node] == s) {
          persists.erase(node);
          exited(node); // Generate ExitedEvent(s)!
        } else if (temps.count(node) > 0 && temps[node] == s) {
          temps.erase(node);
        }

        nodes.erase(s);
      }

      // Clean up any proxy associated with this socket.
      if (proxies.count(s) > 0) {
        proxy = proxies[s];
        proxies.erase(s);
      }

      // We need to stop any 'ignore_data' readers as they may have
      // the last Socket reference so we shutdown reads but don't do a
      // full close (since that will be taken care of by ~Socket, see
      // comment below). Calling 'shutdown' will trigger 'ignore_data'
      // which will get back a 0 (i.e., EOF) when it tries to read
      // from the socket. Note we need to do this before we call
      // 'sockets.erase(s)' to avoid the potential race with the last
      // reference being in 'sockets'.
      shutdown(s, SHUT_RD);

      dispose.erase(s);
      sockets.erase(s);
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
  // reference does a close but force all libev watchers to stop?
}


void SocketManager::exited(const Node& node)
{
  // TODO(benh): It would be cleaner if this routine could call back
  // into ProcessManager ... then we wouldn't have to convince
  // ourselves that the accesses to each Process object will always be
  // valid.
  synchronized (this) {
    list<UPID> removed;
    // Look up all linked processes.
    foreachpair (const UPID& linkee, set<ProcessBase*>& processes, links) {
      if (linkee.ip == node.ip && linkee.port == node.port) {
        foreach (ProcessBase* linker, processes) {
          linker->enqueue(new ExitedEvent(linkee));
        }
        removed.push_back(linkee);
      }
    }

    foreach (const UPID& pid, removed) {
      links.erase(pid);
    }
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

  synchronized (this) {
    // Iterate through the links, removing any links the process might
    // have had and creating exited events for any linked processes.
    foreachpair (const UPID& linkee, set<ProcessBase*>& processes, links) {
      processes.erase(process);

      if (linkee == pid) {
        foreach (ProcessBase* linker, processes) {
          CHECK(linker != process) << "Process linked with itself";
          synchronized (timeouts) {
            if (Clock::paused()) {
              Clock::update(linker, time);
            }
          }
          linker->enqueue(new ExitedEvent(linkee));
        }
      }
    }

    links.erase(pid);
  }
}


ProcessManager::ProcessManager(const string& _delegate)
  : delegate(_delegate)
{
  synchronizer(processes) = SYNCHRONIZED_INITIALIZER_RECURSIVE;
  synchronizer(runq) = SYNCHRONIZED_INITIALIZER_RECURSIVE;
  running = 0;
  __sync_synchronize(); // Ensure write to 'running' visible in other threads.
}


ProcessManager::~ProcessManager() {}


ProcessReference ProcessManager::use(const UPID& pid)
{
  if (pid.ip == __ip__ && pid.port == __port__) {
    synchronized (processes) {
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
      // read the data and parse it as an HTTP request which would
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

  // Split the path by '/'.
  vector<string> tokens = strings::tokenize(request->path, "/");

  // Try and determine a receiver, otherwise try and delegate.
  ProcessReference receiver;

  if (tokens.size() == 0 && delegate != "") {
    request->path = "/" + delegate;
    receiver = use(UPID(delegate, __ip__, __port__));
  } else if (tokens.size() > 0) {
    // Decode possible percent-encoded path.
    Try<string> decode = http::decode(tokens[0]);
    if (!decode.isError()) {
      receiver = use(UPID(decode.get(), __ip__, __port__));
    } else {
      VLOG(1) << "Failed to decode URL path: " << decode.error();
    }
  }

  if (!receiver && delegate != "") {
    // Try and delegate the request.
    request->path = "/" + delegate + request->path;
    receiver = use(UPID(delegate, __ip__, __port__));
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
    synchronized (timeouts) {
      if (Clock::paused()) {
        if (sender != NULL) {
          Clock::order(sender, receiver);
        } else {
          Clock::update(receiver, Clock::now());
        }
      }
    }
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

  delete event;
  return false;
}


UPID ProcessManager::spawn(ProcessBase* process, bool manage)
{
  CHECK(process != NULL);

  synchronized (processes) {
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
      synchronized (filterer) {
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
  synchronized (processes) {
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
  if (!(to.ip == __ip__ && to.port == __port__)) {
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
      synchronized (timeouts) {
        if (Clock::paused()) {
          if (sender != NULL) {
            Clock::order(sender, process);
          } else {
            Clock::update(process, Clock::now());
          }
        }
      }
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
  synchronized (processes) {
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
        synchronized (runq) {
          list<ProcessBase*>::iterator it =
            find(runq.begin(), runq.end(), process);
          if (it != runq.end()) {
            runq.erase(it);
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
    __sync_fetch_and_add(&running, 1);
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


void ProcessManager::enqueue(ProcessBase* process)
{
  CHECK(process != NULL);

  // TODO(benh): Check and see if this process has it's own thread. If
  // it does, push it on that threads runq, and wake up that thread if
  // it's not running. Otherwise, check and see which thread this
  // process was last running on, and put it on that threads runq.

  synchronized (runq) {
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

  synchronized (runq) {
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
    os::sleep(Milliseconds(10));
    done = true;
    // Hopefully this is the only place we acquire both these locks.
    synchronized (runq) {
      synchronized (timeouts) {
        CHECK(Clock::paused()); // Since another thread could resume the clock!

        if (!runq.empty()) {
          done = false;
        }

        __sync_synchronize(); // Read barrier for 'running'.
        if (running > 0) {
          done = false;
        }

        if (timeouts->size() > 0 &&
            timeouts->begin()->first <= clock::current) {
          done = false;
        }

        if (pending_timers) {
          done = false;
        }
      }
    }
  } while (!done);
}


Future<Response> ProcessManager::__processes__(const Request&)
{
  JSON::Array array;

  synchronized (processes) {
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


Timer Timer::create(
    const Duration& duration,
    const lambda::function<void(void)>& thunk)
{
  static uint64_t id = 1; // Start at 1 since Timer() instances use id 0.

  // Assumes Clock::now() does Clock::now(__process__).
  Timeout timeout = Timeout::in(duration);

  UPID pid = __process__ != NULL ? __process__->self() : UPID();

  Timer timer(__sync_fetch_and_add(&id, 1), timeout, pid, thunk);

  VLOG(3) << "Created a timer for " << timeout.time();

  // Add the timer.
  synchronized (timeouts) {
    if (timeouts->size() == 0 ||
        timer.timeout().time() < timeouts->begin()->first) {
      // Need to interrupt the loop to update/set timer repeat.
      (*timeouts)[timer.timeout().time()].push_back(timer);
      update_timer = true;
      ev_async_send(loop, &async_watcher);
    } else {
      // Timer repeat is adequate, just add the timeout.
      CHECK(timeouts->size() >= 1);
      (*timeouts)[timer.timeout().time()].push_back(timer);
    }
  }

  return timer;
}


bool Timer::cancel(const Timer& timer)
{
  bool canceled = false;
  synchronized (timeouts) {
    // Check if the timeout is still pending, and if so, erase it. In
    // addition, erase an empty list if we just removed the last
    // timeout.
    Time time = timer.timeout().time();
    if (timeouts->count(time) > 0) {
      canceled = true;
      (*timeouts)[time].remove(timer);
      if ((*timeouts)[time].empty()) {
        timeouts->erase(time);
      }
    }
  }

  return canceled;
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
  pid.ip = __ip__;
  pid.port = __port__;

  // If using a manual clock, try and set current time of process
  // using happens before relationship between creator and createe!
  if (Clock::paused()) {
    synchronized (timeouts) {
      if (Clock::paused()) {
        clock::currents->erase(this); // In case the address is reused!
        if (__process__ != NULL) {
          Clock::order(__process__, this);
        } else {
          Clock::update(this, Clock::now());
        }
      }
    }
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

  const string& name = tokens.size() > 1 ? tokens[1] : "";

  if (handlers.http.count(name) > 0) {
    // Create the promise to link with whatever gets returned, as well
    // as a future to wait for the response.
    memory::shared_ptr<Promise<Response> > promise(new Promise<Response>());

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


bool ProcessBase::route(
    const string& name,
    const Option<string>& help_,
    const HttpRequestHandler& handler)
{
  if (name.find('/') != 0) {
    return false;
  }
  handlers.http[name.substr(1)] = handler;
  dispatch(help, &Help::add, pid.id, name, help_);
  return true;
}



UPID spawn(ProcessBase* process, bool manage)
{
  process::initialize();

  if (process != NULL) {
    // If using a manual clock, try and set current time of process
    // using happens before relationship between spawner and spawnee!
    if (Clock::paused()) {
      synchronized (timeouts) {
        if (Clock::paused()) {
          if (__process__ != NULL) {
            Clock::order(__process__, process);
          } else {
            Clock::update(process, Clock::now());
          }
        }
      }
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

  synchronized (filterer) {
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


namespace io {

namespace internal {

// Helper/continuation of 'poll' on future discard.
void _poll(const memory::shared_ptr<ev_async>& async)
{
  ev_async_send(loop, async.get());
}


Future<short> poll(int fd, short events)
{
  Poll* poll = new Poll();

  // Have the watchers data point back to the struct.
  poll->watcher.async->data = poll;
  poll->watcher.io->data = poll;

  // Get a copy of the future to avoid any races with the event loop.
  Future<short> future = poll->promise.future();

  // Initialize and start the async watcher.
  ev_async_init(poll->watcher.async.get(), discard_poll);
  ev_async_start(loop, poll->watcher.async.get());

  // Make sure we stop polling if a discard occurs on our future.
  // Note that it's possible that we'll invoke '_poll' when someone
  // does a discard even after the polling has already completed, but
  // in this case while we will interrupt the event loop since the
  // async watcher has already been stopped we won't cause
  // 'discard_poll' to get invoked.
  future.onDiscard(lambda::bind(&_poll, poll->watcher.async));

  // Initialize and start the I/O watcher.
  ev_io_init(poll->watcher.io.get(), polled, fd, events);
  ev_io_start(loop, poll->watcher.io.get());

  return future;
}


void read(
    int fd,
    void* data,
    size_t size,
    const memory::shared_ptr<Promise<size_t> >& promise,
    const Future<short>& future)
{
  // Ignore this function if the read operation has been discarded.
  if (promise->future().hasDiscard()) {
    CHECK(!future.isPending());
    promise->discard();
    return;
  }

  if (size == 0) {
    promise->set(0);
    return;
  }

  if (future.isDiscarded()) {
    promise->fail("Failed to poll: discarded future");
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else {
    ssize_t length = ::read(fd, data, size);
    if (length < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        // Restart the read operation.
        Future<short> future =
          io::poll(fd, process::io::READ).onAny(
              lambda::bind(&internal::read,
                           fd,
                           data,
                           size,
                           promise,
                           lambda::_1));

        // Stop polling if a discard occurs on our future.
        promise->future().onDiscard(
            lambda::bind(&process::internal::discard<short>,
                         WeakFuture<short>(future)));
      } else {
        // Error occurred.
        promise->fail(strerror(errno));
      }
    } else {
      promise->set(length);
    }
  }
}


void write(
    int fd,
    void* data,
    size_t size,
    const memory::shared_ptr<Promise<size_t> >& promise,
    const Future<short>& future)
{
  // Ignore this function if the write operation has been discarded.
  if (promise->future().hasDiscard()) {
    promise->discard();
    return;
  }

  if (size == 0) {
    promise->set(0);
    return;
  }

  if (future.isDiscarded()) {
    promise->fail("Failed to poll: discarded future");
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else {
    // Do a write but ignore SIGPIPE so we can return an error when
    // writing to a pipe or socket where the reading end is closed.
    // TODO(benh): The 'suppress' macro failed to work on OS X as it
    // appears that signal delivery was happening asynchronously.
    // That is, the signal would not appear to be pending when the
    // 'suppress' block was closed thus the destructor for
    // 'Suppressor' was not waiting/removing the signal via 'sigwait'.
    // It also appeared that the signal would be delivered to another
    // thread even if it remained blocked in this thiread. The
    // workaround here is to check explicitly for EPIPE and then do
    // 'sigwait' regardless of what 'os::signals::pending' returns. We
    // don't have that luxury with 'Suppressor' and arbitrary signals
    // because we don't always have something like EPIPE to tell us
    // that a signal is (or will soon be) pending.
    bool pending = os::signals::pending(SIGPIPE);
    bool unblock = !pending ? os::signals::block(SIGPIPE) : false;

    ssize_t length = ::write(fd, data, size);

    // Save the errno so we can restore it after doing sig* functions
    // below.
    int errno_ = errno;

    if (length < 0 && errno == EPIPE && !pending) {
      sigset_t mask;
      sigemptyset(&mask);
      sigaddset(&mask, SIGPIPE);

      int result;
      do {
        int ignored;
        result = sigwait(&mask, &ignored);
      } while (result == -1 && errno == EINTR);
    }

    if (unblock) {
      os::signals::unblock(SIGPIPE);
    }

    errno = errno_;

    if (length < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        // Restart the write operation.
        Future<short> future =
          io::poll(fd, process::io::WRITE).onAny(
              lambda::bind(&internal::write,
                           fd,
                           data,
                           size,
                           promise,
                           lambda::_1));

        // Stop polling if a discard occurs on our future.
        promise->future().onDiscard(
            lambda::bind(&process::internal::discard<short>,
                         WeakFuture<short>(future)));
      } else {
        // Error occurred.
        promise->fail(strerror(errno));
      }
    } else {
      // TODO(benh): Retry if 'length' is 0?
      promise->set(length);
    }
  }
}

} // namespace internal {


Future<short> poll(int fd, short events)
{
  process::initialize();

  // TODO(benh): Check if the file descriptor is non-blocking?

  return run_in_event_loop<short>(lambda::bind(&internal::poll, fd, events));
}


Future<size_t> read(int fd, void* data, size_t size)
{
  process::initialize();

  memory::shared_ptr<Promise<size_t> > promise(new Promise<size_t>());

  // Check the file descriptor.
  Try<bool> nonblock = os::isNonblock(fd);
  if (nonblock.isError()) {
    // The file descriptor is not valid (e.g., has been closed).
    promise->fail(
        "Failed to check if file descriptor was non-blocking: " +
        nonblock.error());
    return promise->future();
  } else if (!nonblock.get()) {
    // The file descriptor is not non-blocking.
    promise->fail("Expected a non-blocking file descriptor");
    return promise->future();
  }

  // Because the file descriptor is non-blocking, we call read()
  // immediately. The read may in turn call poll if necessary,
  // avoiding unnecessary polling. We also observed that for some
  // combination of libev and Linux kernel versions, the poll would
  // block for non-deterministically long periods of time. This may be
  // fixed in a newer version of libev (we use 3.8 at the time of
  // writing this comment).
  internal::read(fd, data, size, promise, io::READ);

  return promise->future();
}


Future<size_t> write(int fd, void* data, size_t size)
{
  process::initialize();

  memory::shared_ptr<Promise<size_t> > promise(new Promise<size_t>());

  // Check the file descriptor.
  Try<bool> nonblock = os::isNonblock(fd);
  if (nonblock.isError()) {
    // The file descriptor is not valid (e.g., has been closed).
    promise->fail(
        "Failed to check if file descriptor was non-blocking: " +
        nonblock.error());
    return promise->future();
  } else if (!nonblock.get()) {
    // The file descriptor is not non-blocking.
    promise->fail("Expected a non-blocking file descriptor");
    return promise->future();
  }

  // Because the file descriptor is non-blocking, we call write()
  // immediately. The write may in turn call poll if necessary,
  // avoiding unnecessary polling. We also observed that for some
  // combination of libev and Linux kernel versions, the poll would
  // block for non-deterministically long periods of time. This may be
  // fixed in a newer version of libev (we use 3.8 at the time of
  // writing this comment).
  internal::write(fd, data, size, promise, io::WRITE);

  return promise->future();
}


namespace internal {

#if __cplusplus >= 201103L
Future<string> _read(
    int fd,
    const memory::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length)
{
  return io::read(fd, data.get(), length)
    .then([=] (size_t size) -> Future<string> {
      if (size == 0) { // EOF.
        return string(*buffer);
      }
      buffer->append(data.get(), size);
      return _read(fd, buffer, data, length);
    });
}
#else
// Forward declataion.
Future<string> _read(
    int fd,
    const memory::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length);


Future<string> __read(
    size_t size,
    int fd,
    const memory::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length)
{
  if (size == 0) { // EOF.
    return string(*buffer);
  }

  buffer->append(data.get(), size);

  return _read(fd, buffer, data, length);
}


Future<string> _read(
    int fd,
    const memory::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length)
{
  return io::read(fd, data.get(), length)
    .then(lambda::bind(&__read, lambda::_1, fd, buffer, data, length));
}
#endif // __cplusplus >= 201103L


#if __cplusplus >= 201103L
Future<Nothing> _write(
    int fd,
    Owned<string> data,
    size_t index)
{
  return io::write(fd, (void*) (data->data() + index), data->size() - index)
    .then([=] (size_t length) -> Future<Nothing> {
      if (index + length == data->size()) {
        return Nothing();
      }
      return _write(fd, data, index + length);
    });
}
#else
// Forward declaration.
Future<Nothing> _write(
    int fd,
    Owned<string> data,
    size_t index);


Future<Nothing> __write(
    int fd,
    Owned<string> data,
    size_t index,
    size_t length)
{
  if (index + length == data->size()) {
    return Nothing();
  }
  return _write(fd, data, index + length);
}


Future<Nothing> _write(
    int fd,
    Owned<string> data,
    size_t index)
{
  return io::write(fd, (void*) (data->data() + index), data->size() - index)
    .then(lambda::bind(&__write, fd, data, index, lambda::_1));
}
#endif // __cplusplus >= 201103L


#if __cplusplus >= 201103L
void _splice(
    int from,
    int to,
    size_t chunk,
    boost::shared_array<char> data,
    memory::shared_ptr<Promise<Nothing>> promise)
{
  // Stop splicing if a discard occured on our future.
  if (promise->future().hasDiscard()) {
    // TODO(benh): Consider returning the number of bytes already
    // spliced on discarded, or a failure. Same for the 'onDiscarded'
    // callbacks below.
    promise->discard();
    return;
  }

  // Note that only one of io::read or io::write is outstanding at any
  // one point in time thus the reuse of 'data' for both operations.

  Future<size_t> read = io::read(from, data.get(), chunk);

  // Stop reading (or potentially indefinitely polling) if a discard
  // occcurs on our future.
  promise->future().onDiscard(
      lambda::bind(&process::internal::discard<size_t>,
                   WeakFuture<size_t>(read)));

  read
    .onReady([=] (size_t size) {
      if (size == 0) { // EOF.
        promise->set(Nothing());
      } else {
        // Note that we always try and complete the write, even if a
        // discard has occured on our future, in order to provide
        // semantics where everything read is written. The promise
        // will eventually be discarded in the next read.
        io::write(to, string(data.get(), size))
          .onReady([=] () { _splice(from, to, chunk, data, promise); })
          .onFailed([=] (const string& message) { promise->fail(message); })
          .onDiscarded([=] () { promise->discard(); });
      }
    })
    .onFailed([=] (const string& message) { promise->fail(message); })
    .onDiscarded([=] () { promise->discard(); });
}
#else
// Forward declarations.
void __splice(
    int from,
    int to,
    size_t chunk,
    boost::shared_array<char> data,
    memory::shared_ptr<Promise<Nothing> > promise,
    size_t size);

void ___splice(
    memory::shared_ptr<Promise<Nothing> > promise,
    const string& message);

void ____splice(
    memory::shared_ptr<Promise<Nothing> > promise);


void _splice(
    int from,
    int to,
    size_t chunk,
    boost::shared_array<char> data,
    memory::shared_ptr<Promise<Nothing> > promise)
{
  // Stop splicing if a discard occured on our future.
  if (promise->future().hasDiscard()) {
    // TODO(benh): Consider returning the number of bytes already
    // spliced on discarded, or a failure. Same for the 'onDiscarded'
    // callbacks below.
    promise->discard();
    return;
  }

  Future<size_t> read = io::read(from, data.get(), chunk);

  // Stop reading (or potentially indefinitely polling) if a discard
  // occurs on our future.
  promise->future().onDiscard(
      lambda::bind(&process::internal::discard<size_t>,
                   WeakFuture<size_t>(read)));

  read
    .onReady(
        lambda::bind(&__splice, from, to, chunk, data, promise, lambda::_1))
    .onFailed(lambda::bind(&___splice, promise, lambda::_1))
    .onDiscarded(lambda::bind(&____splice, promise));
}


void __splice(
    int from,
    int to,
    size_t chunk,
    boost::shared_array<char> data,
    memory::shared_ptr<Promise<Nothing> > promise,
    size_t size)
{
  if (size == 0) { // EOF.
    promise->set(Nothing());
  } else {
    // Note that we always try and complete the write, even if a
    // discard has occured on our future, in order to provide
    // semantics where everything read is written. The promise will
    // eventually be discarded in the next read.
    io::write(to, string(data.get(), size))
      .onReady(lambda::bind(&_splice, from, to, chunk, data, promise))
      .onFailed(lambda::bind(&___splice, promise, lambda::_1))
      .onDiscarded(lambda::bind(&____splice, promise));
  }
}


void ___splice(
    memory::shared_ptr<Promise<Nothing> > promise,
    const string& message)
{
  promise->fail(message);
}


void ____splice(
    memory::shared_ptr<Promise<Nothing> > promise)
{
  promise->discard();
}
#endif // __cplusplus >= 201103L


Future<Nothing> splice(int from, int to, size_t chunk)
{
  boost::shared_array<char> data(new char[chunk]);

  // Rather than having internal::_splice return a future and
  // implementing internal::_splice as a chain of io::read and
  // io::write calls, we use an explicit promise that we pass around
  // so that we don't increase memory usage the longer that we splice.
  memory::shared_ptr<Promise<Nothing> > promise(new Promise<Nothing>());

  Future<Nothing> future = promise->future();

  _splice(from, to, chunk, data, promise);

  return future;
}

} // namespace internal {


Future<string> read(int fd)
{
  process::initialize();

  // Get our own copy of the file descriptor so that we're in control
  // of the lifetime and don't crash if/when someone by accidently
  // closes the file descriptor before discarding this future. We can
  // also make sure it's non-blocking and will close-on-exec. Start by
  // checking we've got a "valid" file descriptor before dup'ing.
  if (fd < 0) {
    return Failure(strerror(EBADF));
  }

  fd = dup(fd);
  if (fd == -1) {
    return Failure(ErrnoError("Failed to duplicate file descriptor"));
  }

  // Set the close-on-exec flag.
  Try<Nothing> cloexec = os::cloexec(fd);
  if (cloexec.isError()) {
    os::close(fd);
    return Failure(
        "Failed to set close-on-exec on duplicated file descriptor: " +
        cloexec.error());
  }

  // Make the file descriptor is non-blocking.
  Try<Nothing> nonblock = os::nonblock(fd);
  if (nonblock.isError()) {
    os::close(fd);
    return Failure(
        "Failed to make duplicated file descriptor non-blocking: " +
        nonblock.error());
  }

  // TODO(benh): Wrap up this data as a struct, use 'Owner'.
  // TODO(bmahler): For efficiency, use a rope for the buffer.
  memory::shared_ptr<string> buffer(new string());
  boost::shared_array<char> data(new char[BUFFERED_READ_SIZE]);

  return internal::_read(fd, buffer, data, BUFFERED_READ_SIZE)
    .onAny(lambda::bind(&os::close, fd));
}


Future<Nothing> write(int fd, const std::string& data)
{
  process::initialize();

  // Get our own copy of the file descriptor so that we're in control
  // of the lifetime and don't crash if/when someone by accidently
  // closes the file descriptor before discarding this future. We can
  // also make sure it's non-blocking and will close-on-exec. Start by
  // checking we've got a "valid" file descriptor before dup'ing.
  if (fd < 0) {
    return Failure(strerror(EBADF));
  }

  fd = dup(fd);
  if (fd == -1) {
    return Failure(ErrnoError("Failed to duplicate file descriptor"));
  }

  // Set the close-on-exec flag.
  Try<Nothing> cloexec = os::cloexec(fd);
  if (cloexec.isError()) {
    os::close(fd);
    return Failure(
        "Failed to set close-on-exec on duplicated file descriptor: " +
        cloexec.error());
  }

  // Make the file descriptor is non-blocking.
  Try<Nothing> nonblock = os::nonblock(fd);
  if (nonblock.isError()) {
    os::close(fd);
    return Failure(
        "Failed to make duplicated file descriptor non-blocking: " +
        nonblock.error());
  }

  return internal::_write(fd, Owned<string>(new string(data)), 0)
    .onAny(lambda::bind(&os::close, fd));
}


Future<Nothing> redirect(int from, Option<int> to, size_t chunk)
{
  // Make sure we've got "valid" file descriptors.
  if (from < 0 || (to.isSome() && to.get() < 0)) {
    return Failure(strerror(EBADF));
  }

  if (to.isNone()) {
    // Open up /dev/null that we can splice into.
    Try<int> open = os::open("/dev/null", O_WRONLY);

    if (open.isError()) {
      return Failure("Failed to open /dev/null for writing: " + open.error());
    }

    to = open.get();
  } else {
    // Duplicate 'to' so that we're in control of its lifetime.
    int fd = dup(to.get());
    if (fd == -1) {
      return Failure(ErrnoError("Failed to duplicate 'to' file descriptor"));
    }

    to = fd;
  }

  CHECK_SOME(to);

  // Duplicate 'from' so that we're in control of its lifetime.
  from = dup(from);
  if (from == -1) {
    return Failure(ErrnoError("Failed to duplicate 'from' file descriptor"));
  }

  // Set the close-on-exec flag (no-op if already set).
  Try<Nothing> cloexec = os::cloexec(from);
  if (cloexec.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to set close-on-exec on 'from': " + cloexec.error());
  }

  cloexec = os::cloexec(to.get());
  if (cloexec.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to set close-on-exec on 'to': " + cloexec.error());
  }

  // Make the file descriptors non-blocking (no-op if already set).
  Try<Nothing> nonblock = os::nonblock(from);
  if (nonblock.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to make 'from' non-blocking: " + nonblock.error());
  }

  nonblock = os::nonblock(to.get());
  if (nonblock.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to make 'to' non-blocking: " + nonblock.error());
  }

  return internal::splice(from, to.get(), chunk)
    .onAny(lambda::bind(&os::close, from))
    .onAny(lambda::bind(&os::close, to.get()));
}

} // namespace io {


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
    const memory::shared_ptr<lambda::function<void(ProcessBase*)> >& f,
    const string& method)
{
  process::initialize();

  DispatchEvent* event = new DispatchEvent(pid, f, method);
  process_manager->deliver(pid, event, __process__);
}

} // namespace internal {
} // namespace process {
