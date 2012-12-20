#include <errno.h>
#include <ev.h>
#include <limits.h>
#include <libgen.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
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

#include <tr1/functional>
#include <tr1/memory> // TODO(benh): Replace all shared_ptr with unique_ptr.

#include <boost/shared_array.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/executor.hpp>
#include <process/filter.hpp>
#include <process/future.hpp>
#include <process/gc.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/mime.hpp>
#include <process/process.hpp>
#include <process/profiler.hpp>
#include <process/socket.hpp>
#include <process/thread.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#include "config.hpp"
#include "decoder.hpp"
#include "encoder.hpp"
#include "gate.hpp"
#include "synchronized.hpp"

using process::wait; // Necessary on some OS's to disambiguate.

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


template <int i>
std::ostream& fixedprecision(std::ostream& os)
{
  return os << std::fixed << std::setprecision(i);
}


// Represents a remote "node" (encapsulates IP address and port).
class Node
{
public:
  Node(uint32_t _ip = 0, uint16_t _port = 0)
    : ip(_ip), port(_port) {}

  bool operator < (const Node& that) const
  {
    if (ip == that.ip) {
      return port < that.port;
    } else {
      return ip < that.ip;
    }
  }

  ostream& operator << (ostream& stream) const
  {
    stream << ip << ":" << port;
    return stream;
  }

  uint32_t ip;
  uint16_t port;
};


namespace process {

namespace ID {

string generate(const string& prefix)
{
  static map<string, int> prefixes;
  stringstream out;
  out << __sync_add_and_fetch(&prefixes[prefix], 1);
  return prefix + "(" + out.str() + ")";
}

} // namespace ID {


namespace http {

hashmap<uint16_t, string> statuses;

} // namespace http {


namespace mime {

map<string, string> types;

} // namespace mime {


// Provides reference counting semantics for a process pointer.
class ProcessReference
{
public:
  ProcessReference() : process(NULL) {}

  ~ProcessReference()
  {
    cleanup();
  }

  ProcessReference(const ProcessReference& that)
  {
    copy(that);
  }

  ProcessReference& operator = (const ProcessReference& that)
  {
    if (this != &that) {
      cleanup();
      copy(that);
    }
    return *this;
  }

  ProcessBase* operator -> ()
  {
    return process;
  }

  operator ProcessBase* ()
  {
    return process;
  }

  operator bool () const
  {
    return process != NULL;
  }

private:
  friend class ProcessManager; // For ProcessManager::use.

  ProcessReference(ProcessBase* _process)
    : process(_process)
  {
    if (process != NULL) {
      __sync_fetch_and_add(&(process->refs), 1);
    }
  }

  void copy(const ProcessReference& that)
  {
    process = that.process;

    if (process != NULL) {
      // There should be at least one reference to the process, so
      // we don't need to worry about checking if it's exiting or
      // not, since we know we can always create another reference.
      CHECK(process->refs > 0);
      __sync_fetch_and_add(&(process->refs), 1);
    }
  }

  void cleanup()
  {
    if (process != NULL) {
      __sync_fetch_and_sub(&(process->refs), 1);
    }
  }

  ProcessBase* process;
};


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
  HttpProxy(const Socket& _socket);
  virtual ~HttpProxy();

  // Enqueues the response to be sent once all previously enqueued
  // responses have been processed (e.g., waited for and sent).
  void enqueue(const Response& response, bool persist);

  // Enqueues a future to a response that will get waited on (up to
  // some timeout) and then sent once all previously enqueued
  // responses have been processed (e.g., waited for and sent).
  void handle(Future<Response>* future, bool persist);

private:
  // Starts "waiting" on the next available future response.
  void next();

  // Invoked once a future response has been satisfied.
  void waited(const Future<Response>& future);

  // Demuxes and handles a response.
  bool process(const Future<Response>& future, bool persist);

  // Handles stream (i.e., pipe) based responses.
  void stream(const Future<short>& poll, bool persist);

  Socket socket; // Wrap the socket to keep it from getting closed.

  // Describes a queue "item" that wraps the future to the response
  // and whether or not the socket should be persisted (vs closed).
  struct Item
  {
    Item(Future<Response>* _future, bool _persist)
      : future(_future), persist(_persist) {}

    ~Item()
    {
      delete future;
    }

    Future<Response>* future;
    bool persist;
  };

  queue<Item*> items;

  Option<int> pipe; // Current pipe, if streaming.
};


class SocketManager
{
public:
  SocketManager();
  ~SocketManager();

  Socket accepted(int s);

  void link(ProcessBase* process, const UPID& to);

  PID<HttpProxy> proxy(int s);

  void send(Encoder* encoder, int s, bool persist);
  void send(const Response& response, int s, bool persist);
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
  ProcessManager(const string& delegate);
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

// Queue of I/O watchers.
static queue<ev_io*>* watchers = new queue<ev_io*>();
static synchronizable(watchers) = SYNCHRONIZED_INITIALIZER;

// We store the timers in a map of lists indexed by the timeout of the
// timer so that we can have two timers that have the same timeout. We
// exploit that the map is SORTED!
static map<double, list<Timer> >* timeouts =
  new map<double, list<Timer> >();
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

// Per thread process pointer.
ThreadLocal<ProcessBase>* _process_ = new ThreadLocal<ProcessBase>();

// Per thread executor pointer.
ThreadLocal<Executor>* _executor_ = new ThreadLocal<Executor>();


// We namespace the clock related variables to keep them well
// named. In the future we'll probably want to associate a clock with
// a specific ProcessManager/SocketManager instance pair, so this will
// likely change.
namespace clock {

map<ProcessBase*, double>* currents = new map<ProcessBase*, double>();

double initial = 0;
double current = 0;

bool paused = false;

} // namespace clock {


double Clock::now()
{
  return now(__process__);
}


double Clock::now(ProcessBase* process)
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
    
  return ev_time(); // TODO(benh): Versus ev_now()?
}


void Clock::pause()
{
  process::initialize(); // To make sure the libev watchers are ready.

  synchronized (timeouts) {
    if (!clock::paused) {
      clock::initial = clock::current = now();
      clock::paused = true;
      VLOG(2) << "Clock paused at " << fixedprecision<9> << clock::initial;
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
      VLOG(2) << "Clock resumed at "
              << std::fixed << std::setprecision(9) << clock::current;
      clock::paused = false;
      clock::currents->clear();
      update_timer = true;
      ev_async_send(loop, &async_watcher);
    }
  }
}


void Clock::advance(double secs)
{
  synchronized (timeouts) {
    if (clock::paused) {
      clock::current += secs;
      VLOG(2) << "Clock advanced ("
              << std::fixed << std::setprecision(9) << secs
              << " seconds) to " << clock::current;
      if (!update_timer) {
        update_timer = true;
        ev_async_send(loop, &async_watcher);
      }
    }
  }
}


void Clock::advance(ProcessBase* process, double secs)
{
  synchronized (timeouts) {
    if (clock::paused) {
      double current = now(process);
      current += secs;
      (*clock::currents)[process] = current;
      VLOG(2) << "Clock of " << process->self() << " advanced ("
              << std::fixed << std::setprecision(9) << secs
              << " seconds) to " << current;
    }
  }
}


void Clock::update(double secs)
{
  synchronized (timeouts) {
    if (clock::paused) {
      if (clock::current < secs) {
        clock::current = secs;
        VLOG(2) << "Clock updated to "
                << std::fixed << std::setprecision(9) << clock::current;
        if (!update_timer) {
          update_timer = true;
          ev_async_send(loop, &async_watcher);
        }
      }
    }
  }
}


void Clock::update(ProcessBase* process, double secs)
{
  synchronized (timeouts) {
    if (clock::paused) {
      double current = now(process);
      if (current < secs) {
        VLOG(2) << "Clock of " << process->self() << " updated to "
                << std::fixed << std::setprecision(9) << secs;
        (*clock::currents)[process] = secs;
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
  return request->method == "POST" &&
    request->headers.count("User-Agent") > 0 &&
    request->headers["User-Agent"].find("libprocess/") == 0;
}


static Message* parse(Request* request)
{
  // TODO(benh): Do better error handling (to deal with a malformed
  // libprocess message, malicious or otherwise).
  const string& agent = request->headers["User-Agent"];
  const string& identifier = "libprocess/";
  size_t index = agent.find(identifier);
  if (index != string::npos) {
    // Okay, now determine 'from'.
    const UPID from(agent.substr(index + identifier.size(), agent.size()));

    // Now determine 'to'.
    index = request->path.find('/', 1);
    index = index != string::npos ? index - 1 : string::npos;
    const UPID to(request->path.substr(1, index), __ip__, __port__);

    // And now determine 'name'.
    index = index != string::npos ? index + 2: request->path.size();
    const string& name = request->path.substr(index);

    VLOG(2) << "Parsed message name '" << name
            << "' for " << to << " from " << from;

    Message* message = new Message();
    message->name = name;
    message->from = from;
    message->to = to;
    message->body = request->body;

    return message;
  }

  return NULL;
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
  }

  synchronized (timeouts) {
    if (update_timer) {
      if (!timeouts->empty()) {
	// Determine when the next timer should fire.
	timeouts_watcher.repeat = timeouts->begin()->first - Clock::now();

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
    double now = Clock::now();

    VLOG(3) << "Handling timeouts up to "
            << std::fixed << std::setprecision(9) << now;

    double timeout;
    foreachkey (timeout, *timeouts) {
      if (timeout > now) {
        break;
      }

      VLOG(3) << "Have timeout(s) at "
              << std::fixed << std::setprecision(9) << timeout;

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
      timeouts_watcher.repeat = timeouts->begin()->first - Clock::now();

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
        Clock::update(process, timer.timeout().value());
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
        VLOG(1) << "Socket closed while receiving";
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

    ssize_t length = sendfile(s, fd, offset, size);

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
    DataDecoder* decoder = (DataDecoder*) watcher->data;
    delete decoder;
    ev_io_stop(loop, watcher);
    delete watcher;
  } else {
    // We're connected! Now let's do some receiving.
    ev_io_stop(loop, watcher);
    ev_io_init(watcher, recv_data, s, EV_READ);
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
    close(s);
    return;
  }

  Try<Nothing> cloexec = os::cloexec(s);
  if (cloexec.isError()) {
    LOG_IF(INFO, VLOG_IS_ON(1)) << "Failed to accept, cloexec: "
                                << cloexec.error();
    close(s);
    return;
  }

  // Turn off Nagle (TCP_NODELAY) so pipelined requests don't wait.
  int on = 1;
  if (setsockopt(s, SOL_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    const char* error = strerror(errno);
    VLOG(1) << "Failed to turn off the Nagle algorithm: " << error;
    close(s);
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


void polled(struct ev_loop* loop, ev_io* watcher, int revents)
{
  Promise<short>* promise = (Promise<short>*) watcher->data;
  promise->set(revents);
  delete promise;

  ev_io_stop(loop, watcher);
  delete watcher;
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
  long cpus = std::max(4L, sysconf(_SC_NPROCESSORS_ONLN));

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
  if ((__s__ = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
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
    PLOG(FATAL) << "Failed to initialize, bind";
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
      PLOG(FATAL) << "Ffailed to initialize, gethostname";
    }

    // Lookup IP address of local hostname.
    hostent* he;

    if ((he = gethostbyname2(hostname, AF_INET)) == NULL) {
      PLOG(FATAL) << "Failed to initialize, gethostbyname2";
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

  // Create global garbage collector.
  gc = spawn(new GarbageCollector());

  // Create the global profiler.
  spawn(new Profiler(), true);

  // Initialize the mime types.
  mime::initialize();

  // Initialize the response statuses.
  http::initialize();

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
    close(pipe.get());
  }
  pipe = Option<int>::none();

  while (!items.empty()) {
    Item* item = items.front();

    // Attempt to discard the future.
    item->future->discard();

    // But it might have already been ready ...
    if (item->future->isReady()) {
      const Response& response = item->future->get();
      if (response.type == Response::PIPE) {
        close(response.pipe);
      }
    }

    items.pop();
    delete item;
  }
}


void HttpProxy::enqueue(const Response& response, bool persist)
{
  handle(new Future<Response>(response), persist);
}


void HttpProxy::handle(Future<Response>* future, bool persist)
{
  items.push(new Item(future, persist));

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
  bool processed = process(*item->future, item->persist);

  items.pop();
  delete item;

  if (processed) {
    next();
  }
}


bool HttpProxy::process(const Future<Response>& future, bool persist)
{
  if (!future.isReady()) {
    // TODO(benh): Consider handling other "states" of future
    // (discarded, failed, etc) with different HTTP statuses.
    socket_manager->send(ServiceUnavailable(), socket, persist);
    return true; // All done, can process next response.
  }

  Response response = future.get();

  // Don't persist connection if headers include 'Connection: close'.
  if (response.headers.count("Connection") > 0) {
    const string& connection = response.headers.find("Connection")->second;
    if (connection == "close") {
      persist = false;
    }
  }

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
          socket_manager->send(NotFound(), socket, persist);
      } else {
        const char* error = strerror(errno);
        VLOG(1) << "Failed to send file at '" << path << "': " << error;
        socket_manager->send(InternalServerError(), socket, persist);
      }
    } else {
      struct stat s; // Need 'struct' because of function named 'stat'.
      if (fstat(fd, &s) != 0) {
        const char* error = strerror(errno);
        VLOG(1) << "Failed to send file at '" << path << "': " << error;
        socket_manager->send(InternalServerError(), socket, persist);
      } else if (S_ISDIR(s.st_mode)) {
        VLOG(1) << "Returning '404 Not Found' for directory '" << path << "'";
        socket_manager->send(NotFound(), socket, persist);
      } else {
        // While the user is expected to properly set a 'Content-Type'
        // header, we fill in (or overwrite) 'Content-Length' header.
        stringstream out;
        out << s.st_size;
        response.headers["Content-Length"] = out.str();

        if (s.st_size == 0) {
          socket_manager->send(response, socket, persist);
          return true; // All done, can process next request.
        }

        VLOG(1) << "Sending file at '" << path << "' with length " << s.st_size;

        // TODO(benh): Consider a way to have the socket manager turn
        // on TCP_CORK for both sends and then turn it off.
        socket_manager->send(response, socket, true);

        // Note the file descriptor gets closed by FileEncoder.
        Encoder* encoder = new FileEncoder(fd, s.st_size);
        socket_manager->send(encoder, socket, persist);
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
      socket_manager->send(InternalServerError(), socket, persist);
      return true; // All done, can process next response.
    }

    // While the user is expected to properly set a 'Content-Type'
    // header, we fill in (or overwrite) 'Transfer-Encoding' header.
    response.headers["Transfer-Encoding"] = "chunked";

    VLOG(1) << "Starting \"chunked\" streaming";

    socket_manager->send(response, socket, true);

    pipe = response.pipe;

    io::poll(pipe.get(), io::READ).onAny(
        defer(self(), &Self::stream, lambda::_1, persist));

    return false; // Streaming, don't process next response (yet)!
  } else {
    socket_manager->send(response, socket, persist);
  }

  return true; // All done, can process next response.
}


void HttpProxy::stream(const Future<short>& poll, bool persist)
{
  // TODO(benh): Use 'splice' on Linux.

  CHECK(pipe.isSome());

  bool finished = false; // Whether or not we're done streaming.

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
            defer(self(), &Self::stream, lambda::_1, persist));
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
        socket_manager->send(new DataEncoder(out.str()), socket, persist);
      }
    }
  } else if (poll.isFailed()) {
    VLOG(1) << "Failed to poll: " << poll.failure();
    socket_manager->send(InternalServerError(), socket, persist);
    finished = true;
  } else {
    VLOG(1) << "Unexpected discarded future while polling";
    socket_manager->send(InternalServerError(), socket, persist);
    finished = true;
  }

  if (finished) {
    close(pipe.get());
    pipe = Option<int>::none();
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
      // Okay, no link, lets create a socket.
      int s;

      if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        PLOG(FATAL) << "Failed to link, socket";
      }

      Try<Nothing> nonblock = os::nonblock(s);
      if (nonblock.isError()) {
        LOG(FATAL) << "Failed to link, nonblock: " << nonblock.error();
      }

      Try<Nothing> cloexec = os::cloexec(s);
      if (cloexec.isError()) {
        LOG(FATAL) << "Failed to link, cloexec: " << cloexec.error();
      }

      Socket socket = Socket(s);

      sockets[s] = socket;
      nodes[s] = node;

      persists[node] = s;

      sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = PF_INET;
      addr.sin_port = htons(to.port);
      addr.sin_addr.s_addr = to.ip;

      // Allocate and initialize the decoder and watcher (we really
      // only "receive" on this socket so that we can react when it
      // gets closed and generate appropriate lost events).
      DataDecoder* decoder = new DataDecoder(socket);

      ev_io* watcher = new ev_io();
      watcher->data = decoder;

      // Try and connect to the node using this socket.
      if (connect(s, (sockaddr*) &addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
          PLOG(FATAL) << "Failed to link, connect";
        }

        // Wait for socket to be connected.
        ev_io_init(watcher, receiving_connect, s, EV_WRITE);
      } else {
        ev_io_init(watcher, recv_data, s, EV_READ);
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


PID<HttpProxy> SocketManager::proxy(int s)
{
  HttpProxy* proxy = NULL;

  synchronized (this) {
    // This socket might have been asked to get closed (e.g., remote
    // side hang up) while a process is attempting to handle an HTTP
    // request. Thus, if there is no more socket, return an empty PID.
    if (sockets.count(s) > 0) {
      if (proxies.count(s) > 0) {
        return proxies[s]->self();
      } else {
        proxy = new HttpProxy(sockets[s]);
        proxies[s] = proxy;
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


void SocketManager::send(Encoder* encoder, int s, bool persist)
{
  CHECK(encoder != NULL);

  synchronized (this) {
    if (sockets.count(s) > 0) {
      // Update whether or not this socket should get disposed after
      // there is no more data to send.
      if (!persist) {
        dispose.insert(s);
      }

      if (outgoing.count(s) > 0) {
        outgoing[s].push(encoder);
      } else {
        // Initialize the outgoing queue.
        outgoing[s];

        // Allocate and initialize the watcher.
        ev_io* watcher = new ev_io();
        watcher->data = encoder;

        ev_io_init(watcher, encoder->sender(), s, EV_WRITE);

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


void SocketManager::send(const Response& response, int s, bool persist)
{
  send(new HttpResponseEncoder(response), s, persist);
}


void SocketManager::send(Message* message)
{
  CHECK(message != NULL);

  DataEncoder* encoder = new MessageEncoder(message);

  Node node(message->to.ip, message->to.port);

  synchronized (this) {
    // Check if there is already a socket.
    bool persist = persists.count(node) > 0;
    bool temp = temps.count(node) > 0;
    if (persist || temp) {
      int s = persist ? persists[node] : temps[node];
      send(encoder, s, persist);
    } else {
      // No peristant or temporary socket to the node currently
      // exists, so we create a temporary one.
      int s;

      if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        PLOG(FATAL) << "Failed to send, socket";
      }

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

      // Try and connect to the node using this socket.
      sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = PF_INET;
      addr.sin_port = htons(message->to.port);
      addr.sin_addr.s_addr = message->to.ip;

      // Allocate and initialize the watcher.
      ev_io* watcher = new ev_io();
      watcher->data = encoder;

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
  const double secs = Clock::now(process);

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
              Clock::update(linker, secs);
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
      delete request;
      // TODO(benh): Use the sender PID in order to capture
      // happens-before timing relationships for testing.
      return deliver(message->to, new MessageEvent(message));
    }

    VLOG(1) << "Failed to handle libprocess request: "
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
    dispatch(proxy, &HttpProxy::enqueue, BadRequest(), request->keepAlive);

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
    dispatch(proxy, &HttpProxy::enqueue, NotFound(), request->keepAlive);

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
    receiver = use(UPID(tokens[0], __ip__, __port__));
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
  dispatch(proxy, &HttpProxy::enqueue, NotFound(), request->keepAlive);

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

  VLOG(2) << "Resuming " << process->pid << " at "
          << std::fixed << std::setprecision(9) << Clock::now();

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

  // Processes that were waiting on exiting process.
  list<ProcessBase*> resumable;

  // Possible gate non-libprocess threads are waiting at.
  Gate* gate = NULL;
 
  // Remove process.
  synchronized (processes) {
    // Wait for all process references to get cleaned up.
    while (process->refs > 0) {
      asm ("pause");
      __sync_synchronize();
    }

    process->lock();
    {
      // Free any pending events.
      while (!process->events.empty()) {
        Event* event = process->events.front();
        process->events.pop_front();
        delete event;
      }

      processes.erase(process->pid.id);
 
      // Lookup gate to wake up waiting threads.
      map<ProcessBase*, Gate*>::iterator it = gates.find(process);
      if (it != gates.end()) {
        gate = it->second;
        // N.B. The last thread that leaves the gate also free's it.
        gates.erase(it);
      }

      CHECK(process->refs == 0);
      process->state = ProcessBase::FINISHED;
    }
    process->unlock();

    // Note that we don't remove the process from the clock during
    // cleanup, but rather the clock is reset for a process when it is
    // created (see ProcessBase::ProcessBase). We do this so that
    // SocketManager::exited can access the current time of the
    // process to "order" exited events. It might make sense to
    // consider storing the time of the process as a field of the
    // class instead.

    // Now we tell the socket manager about this process exiting so
    // that it can create exited events for linked processes. We
    // _must_ do this while synchronized on processes because
    // otherwise another process could attempt to link this process
    // and SocketManger::link would see that the processes doesn't
    // exist when it attempts to get a ProcessReference (since we
    // removed the process above) thus causing an exited event, which
    // could cause the process to get deleted (e.g., the garbage
    // collector might link _after_ the process has already been
    // removed, thus getting an exited event but we don't want that
    // exited event to fire until after we have used the process in
    // SocketManager::exited.
    socket_manager->exited(process);
  }

  // ***************************************************************
  // At this point we can no longer dereference the process since it
  // might already be deallocated (e.g., by the garbage collector).
  // ***************************************************************

  if (gate != NULL) {
    gate->open();
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
      CHECK(process->state != ProcessBase::FINISHED);

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
    usleep(10000);
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


Timer Timer::create(
    const Duration& duration,
    const lambda::function<void(void)>& thunk)
{
  static uint64_t id = 1; // Start at 1 since Timer() instances start with 0.

  Timeout timeout(duration); // Assumes Clock::now() does Clock::now(__process__).

  UPID pid = __process__ != NULL ? __process__->self() : UPID();

  Timer timer(__sync_fetch_and_add(&id, 1), timeout, pid, thunk);

  VLOG(3) << "Created a timer for "
          << std::fixed << std::setprecision(9) << timeout.value();

  // Add the timer.
  synchronized (timeouts) {
    if (timeouts->size() == 0 ||
        timer.timeout().value() < timeouts->begin()->first) {
      // Need to interrupt the loop to update/set timer repeat.
      (*timeouts)[timer.timeout().value()].push_back(timer);
      update_timer = true;
      ev_async_send(loop, &async_watcher);
    } else {
      // Timer repeat is adequate, just add the timeout.
      CHECK(timeouts->size() >= 1);
      (*timeouts)[timer.timeout().value()].push_back(timer);
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
    // TODO(benh): If two timers are created with the same timeout,
    // this will erase *both*. Fix this!
    if (timeouts->count(timer.timeout().value()) > 0) {
      canceled = true;
      (*timeouts)[timer.timeout().value()].remove(timer);
      if ((*timeouts)[timer.timeout().value()].empty()) {
        timeouts->erase(timer.timeout().value());
      }
    }
  }

  return canceled;
}


ProcessBase::ProcessBase(const string& id)
{
  process::initialize();

  state = ProcessBase::BOTTOM;

  pthread_mutex_init(&m, NULL);

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

  // TODO(benh): Put filter inside lock statement below so that we can
  // guarantee the order of the messages seen by a filter are the same
  // as the order of messages seen by the process. Right now two
  // different threads might execute the filter code and then enqueue
  // the messages in non-deterministic orderings (i.e., there are two
  // "atomic" blocks, the filter code here and the enqueue code
  // below).
  synchronized (filterer) {
    if (filterer != NULL) {
      bool filter = false;
      struct FilterVisitor : EventVisitor
      {
        FilterVisitor(bool* _filter) : filter(_filter) {}

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
        return;
      }
    }
  }

  lock();
  {
    if (state != FINISHED) {
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


void ProcessBase::inject(const UPID& from, const string& name, const char* data, size_t length)
{
  if (!from)
    return;

  Message* message = encode(from, pid, name, string(data, length));

  enqueue(new MessageEvent(message), true);
}


void ProcessBase::send(const UPID& to, const string& name, const char* data, size_t length)
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
  CHECK(tokens[0] == pid.id);

  const string& name = tokens.size() > 1 ? tokens[1] : "";

  if (handlers.http.count(name) > 0) {
    // Create the promise to link with whatever gets returned, as well
    // as a future to wait for the response.
    std::tr1::shared_ptr<Promise<Response> > promise(
        new Promise<Response>());

    Future<Response>* future = new Future<Response>(promise->future());

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(event.socket);

    // Let the HttpProxy know about this request (via the future).
    dispatch(proxy, &HttpProxy::handle, future, event.request->keepAlive);

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
    dispatch(proxy, &HttpProxy::enqueue, response, event.request->keepAlive);
  } else {
    VLOG(1) << "Returning '404 Not Found' for '" << event.request->path << "'";

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(event.socket);

    // Enqueue the response with the HttpProxy so that it respects the
    // order of requests to account for HTTP/1.1 pipelining.
    dispatch(proxy, &HttpProxy::enqueue,
             NotFound(), event.request->keepAlive);
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

  if (duration == Seconds(-1.0)) {
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


namespace io {

namespace internal {

void read(int fd,
          void *data,
          size_t size,
          const std::tr1::shared_ptr<Promise<size_t> >& promise,
          const Future<short>& future)
{
  // Ignore this function if the read operation has been cancelled.
  if (promise->future().isDiscarded()) {
    return;
  }

  // Since promise->future() will be discarded before future is discarded, we
  // should never see a discarded future here because of the check in the
  // beginning of this function.
  CHECK(!future.isDiscarded());

  if (future.isFailed()) {
    promise->fail(future.failure());
  } else {
    ssize_t length = ::read(fd, data, size);
    if (length < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        // Restart the read operation.
        poll(fd, process::io::READ).onAny(
            lambda::bind(&internal::read, fd, data, size, promise, lambda::_1));
      } else {
        // Error occurred.
        promise->fail(strerror(errno));
      }
    } else {
      promise->set(length);
    }
  }
}

} // namespace internal {


Future<short> poll(int fd, short events)
{
  process::initialize();

  // TODO(benh): Check if the file descriptor is non-blocking?

  Promise<short>* promise = new Promise<short>();

  // Get a copy of the future to avoid any races with the event loop.
  Future<short> future = promise->future();

  ev_io* watcher = new ev_io();
  watcher->data = promise;

  ev_io_init(watcher, polled, fd, events);

  // Enqueue the watcher.
  synchronized (watchers) {
    watchers->push(watcher);
  }

  // Interrupt the loop.
  ev_async_send(loop, &async_watcher);

  return future;
}


Future<size_t> read(int fd, void* data, size_t size)
{
  process::initialize();

  std::tr1::shared_ptr<Promise<size_t> > promise(new Promise<size_t>());

  // Check the file descriptor.
  Try<bool> nonblock = os::isNonblock(fd);
  if (nonblock.isError()) {
    // The file descriptor is not valid (e.g. fd has been closed).
    promise->fail(strerror(errno));
    return promise->future();
  } else if (!nonblock.get()) {
    // The fd is not opened with O_NONBLOCK set.
    promise->fail("Please use a fd opened with O_NONBLOCK set");
    return promise->future();
  }

  if (size == 0) {
    promise->fail("Try to read nothing");
    return promise->future();
  }

  Future<short> future = poll(fd, process::io::READ).onAny(
      lambda::bind(&internal::read, fd, data, size, promise, lambda::_1));

  // Also cancel the polling if promise->future() is discarded.
  promise->future().onDiscarded(
      lambda::bind(&Future<short>::discard, future));

  return promise->future();
}

namespace internal {

#if __cplusplus >= 201103L
Future<string> _read(int fd,
                     const std::tr1::shared_ptr<string>& buffer,
                     const boost::shared_array<char>& data,
                     size_t length)
{
  return io::read(fd, data.get(), length)
    .then([=] (size_t size) {
      if (size == 0) { // EOF.
        string result(*buffer);
        return Future<string>(result);
      }
      buffer->append(data, size);
      return _read(fd, buffer, data, length);
    });
}
#else
// Forward declataion.
Future<string> _read(int fd,
                     const std::tr1::shared_ptr<string>& buffer,
                     const boost::shared_array<char>& data,
                     size_t length);


Future<string> __read(
    const size_t& size,
    // TODO(benh): Remove 'const &' after fixing libprocess.
    int fd,
    const std::tr1::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length)
{
  if (size == 0) { // EOF.
    string result(*buffer);
    return Future<string>(result);
  }

  buffer->append(data.get(), size);
  return _read(fd, buffer, data, length);
}


Future<string> _read(int fd,
                     const std::tr1::shared_ptr<string>& buffer,
                     const boost::shared_array<char>& data,
                     size_t length)
{
  std::tr1::function<Future<string>(const size_t&)> f =
    std::tr1::bind(__read, lambda::_1, fd, buffer, data, length);

  return io::read(fd, data.get(), length).then(f);
}
#endif

} // namespace internal


Future<string> read(int fd)
{
  process::initialize();

  // TODO(benh): Wrap up this data as a struct, use 'Owner'.
  // TODO(bmahler): For efficiency, use a rope for the buffer.
  std::tr1::shared_ptr<string> buffer(new string());
  boost::shared_array<char> data(new char[BUFFERED_READ_SIZE]);

  return internal::_read(fd, buffer, data, BUFFERED_READ_SIZE);
}


} // namespace io {


namespace http {

namespace internal {

Future<Response> decode(const string& buffer)
{
  ResponseDecoder decoder;
  deque<Response*> responses = decoder.decode(buffer.c_str(), buffer.length());

  if (decoder.failed() || responses.empty()) {
    for (size_t i = 0; i < responses.size(); ++i) {
      delete responses[i];
    }
    return Future<Response>::failed(
        "Failed to decode HTTP response:\n" + buffer + "\n");
  } else if (responses.size() > 1) {
    PLOG(ERROR) << "Received more than 1 HTTP Response";
  }

  Response response = *responses[0];
  for (size_t i = 0; i < responses.size(); ++i) {
    delete responses[i];
  }

  return response;
}

} // namespace internal {


Future<Response> get(const PID<>& pid, const string& path, const string& query)
{
  int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

  if (s < 0) {
    return Future<Response>::failed(
        string("Failed to create socket: ") + strerror(errno));
  }

  Try<Nothing> cloexec = os::cloexec(s);
  if (!cloexec.isSome()) {
    return Future<Response>::failed("Failed to cloexec: " + cloexec.error());
  }

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(pid.port);
  addr.sin_addr.s_addr = pid.ip;

  if (connect(s, (sockaddr*) &addr, sizeof(addr)) < 0) {
    return Future<Response>::failed(strerror(errno));
  }

  std::ostringstream out;

  out << "GET /" << pid.id << "/" << path << "?" << query << " HTTP/1.1\r\n"
      << "Connection: close\r\n"
      << "\r\n";

  // TODO(bmahler): Use benh's async write when it gets committed.
  const string& data = out.str();
  int remaining = data.size();

  while (remaining > 0) {
    int n = write(s, data.data() + (data.size() - remaining), remaining);

    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Future<Response>::failed(strerror(errno));
    }

    remaining -= n;
  }

  Try<Nothing> nonblock = os::nonblock(s);
  if (!nonblock.isSome()) {
    return Future<Response>::failed(
        "Failed to set nonblock: " + nonblock.error());
  }

  // Decode once the async read completes.
  std::tr1::function<Future<Response>(const string&)> decode =
    std::tr1::bind(internal::decode, lambda::_1);

  return io::read(s).then(decode);
}

}  // namespace http {

namespace internal {

void dispatch(
    const UPID& pid,
    const std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> >& f)
{
  process::initialize();

  process_manager->deliver(pid, new DispatchEvent(f), __process__);
}

} // namespace internal {
} // namespace process {
