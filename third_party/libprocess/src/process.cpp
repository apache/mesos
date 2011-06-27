#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
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

#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <vector>

#include <process/dispatch.hpp>
#include <process/gc.hpp>
#include <process/process.hpp>

#include <boost/tuple/tuple.hpp>

#include "config.hpp"
#include "decoder.hpp"
#include "encoder.hpp"
#include "fatal.hpp"
#include "foreach.hpp"
#include "gate.hpp"
#include "synchronized.hpp"
#include "tokenize.hpp"


using boost::tuple;

using std::deque;
using std::find;
using std::list;
using std::map;
using std::max;
using std::ostream;
using std::pair;
using std::queue;
using std::set;
using std::stack;
using std::string;
using std::stringstream;
using std::vector;

using std::tr1::function;


#define Byte (1)
#define Kilobyte (1024*Byte)
#define Megabyte (1024*Kilobyte)
#define Gigabyte (1024*Megabyte)
#define PROCESS_STACK_SIZE (64*Kilobyte)


#define malloc(bytes)                                               \
  ({ void *tmp;                                                     \
     if ((tmp = malloc(bytes)) == NULL)                             \
       fatalerror("malloc"); tmp;                                   \
   })

#define realloc(address, bytes)                                     \
  ({ void *tmp;                                                     \
     if ((tmp = realloc(address, bytes)) == NULL)                   \
       fatalerror("realloc"); tmp;                                  \
   })


struct Node
{
  Node(uint32_t _ip = 0, uint16_t _port = 0)
    : ip(_ip), port(_port) {}

  uint32_t ip;
  uint16_t port;
};


bool operator < (const Node& left, const Node& right)
{
  if (left.ip == right.ip)
    return left.port < right.port;
  else
    return left.ip < right.ip;
}


ostream& operator << (ostream& stream, const Node& node)
{
  stream << node.ip << ":" << node.port;
  return stream;
}


/*
 * Timeout support! Note that we don't store a pointer to the process
 * because we can't dereference it because it might no longer be
 * valid. But we can check if the process is valid using the PID and
 * then use referencing counting to keep the process valid.
*/
struct timeout
{
  ev_tstamp tstamp;
  process::UPID pid;
  int generation;
};


bool operator == (const timeout &left, const timeout &right)
{
  return left.tstamp == right.tstamp &&
    left.pid == right.pid &&
    left.generation == right.generation;
}


namespace process {

class ProcessReference
{
public:
  explicit ProcessReference(ProcessBase *_process) : process(_process)
  {
    if (process != NULL) {
      __sync_fetch_and_add(&(process->refs), 1);
      if (process->state == ProcessBase::FINISHING) {
        __sync_fetch_and_sub(&(process->refs), 1);
        process = NULL;
      }
    }
  }

  ~ProcessReference()
  {
    if (process != NULL)
      __sync_fetch_and_sub(&(process->refs), 1);
  }

  ProcessReference(const ProcessReference &that)
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

  ProcessBase * operator -> ()
  {
    return process;
  }

  operator ProcessBase * ()
  {
    return process;
  }

  operator bool () const
  {
    return process != NULL;
  }

private:
  ProcessReference & operator = (const ProcessReference &that);

  ProcessBase *process;
};


/* Tick, tock ... manually controlled clock! */
class InternalClock
{
public:
  InternalClock()
  {
    initial = current = elapsed = ev_time();
  }

  ~InternalClock() {}

  ev_tstamp getCurrent(ProcessBase *process)
  {
    ev_tstamp tstamp;

    if (currents.count(process) != 0) {
      tstamp = currents[process];
    } else {
      tstamp = currents[process] = initial;
    }

    return tstamp;
  }

  void setCurrent(ProcessBase *process, ev_tstamp tstamp)
  {
    currents[process] = tstamp;
  }

  ev_tstamp getCurrent()
  {
    return current;
  }

  void setCurrent(ev_tstamp tstamp)
  {
    current = tstamp;
  }

  ev_tstamp getElapsed()
  {
    return elapsed;
  }

  void setElapsed(ev_tstamp tstamp)
  {
    elapsed = tstamp;
  }

  void discard(ProcessBase *process)
  {
    CHECK(process != NULL);
    currents.erase(process);
  }

private:
  map<ProcessBase *, ev_tstamp> currents;
  ev_tstamp initial;
  ev_tstamp current;
  ev_tstamp elapsed;
};


class HttpProxy;


class HttpResponseWaiter : public Process<HttpResponseWaiter>
{
public:
  HttpResponseWaiter(const PID<HttpProxy>& _proxy);
  virtual ~HttpResponseWaiter();

  void await(const Future<HttpResponse>& future, bool persist);

private:
  const PID<HttpProxy> proxy;
};


class HttpProxy : public Process<HttpProxy>
{
public:
  HttpProxy(int _c);
  virtual ~HttpProxy();

  void handle(const Future<HttpResponse>& future, bool persist);
  void ready(const Future<HttpResponse>& future, bool persist);
  void unavailable(bool persist);

private:
  int c;
  HttpResponseWaiter* waiter;
};


class SocketManager
{
public:
  SocketManager();
  ~SocketManager();

  void link(ProcessBase* process, const UPID& to);

  PID<HttpProxy> proxy(int s);

  void send(DataEncoder* encoder, int s, bool persist);
  void send(Message* message);

  DataEncoder* next(int s);

  void closed(int s);

  void exited(const Node& node);
  void exited(ProcessBase* process);

private:
  /* Map from UPID (local/remote) to process. */
  map<UPID, set<ProcessBase*> > links;

  /* Map from socket to node (ip, port). */
  map<int, Node> sockets;

  /* Maps from node (ip, port) to socket. */
  map<Node, int> temps;
  map<Node, int> persists;

  /* Set of sockets that should be closed. */
  set<int> disposables;

  /* Map from socket to outgoing queue. */
  map<int, queue<DataEncoder*> > outgoing;

  /* HTTP proxies. */
  map<int, HttpProxy*> proxies;

  /* Protects instance variables. */
  synchronizable(this);
};


class ProcessManager
{
public:
  ProcessManager();
  ~ProcessManager();

  ProcessReference use(const UPID &pid);

  bool deliver(Message* message, ProcessBase *sender = NULL);
  bool deliver(int c, HttpRequest* request, ProcessBase *sender = NULL);
  bool deliver(const UPID& to, function<void(ProcessBase*)>* dispatcher, ProcessBase *sender = NULL);

  UPID spawn(ProcessBase *process, bool manage);
  void link(ProcessBase *process, const UPID &to);
  bool receive(ProcessBase *process, double secs);
  bool serve(ProcessBase *process, double secs);
  void pause(ProcessBase *process, double secs);
  void terminate(const UPID& pid, bool inject, ProcessBase* sender = NULL);
  bool wait(ProcessBase *process, const UPID &pid);
  bool external_wait(const UPID &pid);
  bool poll(ProcessBase *process, int fd, int op, double secs, bool ignore);

  void enqueue(ProcessBase *process);
  ProcessBase * dequeue();

  void timedout(const UPID &pid, int generation);
  void polled(const UPID &pid, int generation);

  void run(ProcessBase *process);
  void cleanup(ProcessBase *process);

  static void invoke(const function<void (void)> &thunk);

private:
  timeout create_timeout(ProcessBase *process, double secs);
  void start_timeout(const timeout &timeout);
  void cancel_timeout(const timeout &timeout);

  /* Map of all local spawned and running processes. */
  map<string, ProcessBase *> processes;
  synchronizable(processes);

  /* Waiting processes (protected by synchronizable(processes)). */
  map<ProcessBase *, set<ProcessBase *> > waiters;

  /* Gates for waiting threads (protected by synchronizable(processes)). */
  map<ProcessBase *, Gate *> gates;

  /* Queue of runnable processes (implemented as deque). */
  deque<ProcessBase *> runq;
  synchronizable(runq);
};


/* Using manual clock if non-null. */
static InternalClock *clk = NULL;

/* Unique id that can be assigned to each process. */
static uint32_t id = 0;

/* Local server socket. */
static int s = -1;

/* Local IP address. */
static uint32_t ip = 0;

/* Local port. */
static uint16_t port = 0;

/* Active SocketManager (eventually will probably be thread-local). */
static SocketManager *socket_manager = NULL;

/* Active ProcessManager (eventually will probably be thread-local). */
static ProcessManager *process_manager = NULL;

/* Event loop. */
static struct ev_loop *loop = NULL;

/* Asynchronous watcher for interrupting loop. */
static ev_async async_watcher;

/* Timeouts watcher for process timeouts. */
static ev_timer timeouts_watcher;

/* Server watcher for accepting connections. */
static ev_io server_watcher;

/* Queue of I/O watchers. */
static queue<ev_io *> *watchers = new queue<ev_io *>();
static synchronizable(watchers) = SYNCHRONIZED_INITIALIZER;

/**
 * We store the timeouts in a map of lists indexed by the time stamp
 * of the timeout so that we can have two timeouts that have the same
 * time stamp. Note however, that we should never have two identical
 * timeouts because a process should only ever have one outstanding
 * timeout at a time. Also, we exploit that the map is SORTED!
 */
static map<ev_tstamp, list<timeout> > *timeouts =
  new map<ev_tstamp, list<timeout> >();
static synchronizable(timeouts) = SYNCHRONIZED_INITIALIZER;

/* Flag to indicate whether or to update the timer on async interrupt. */
static bool update_timer = false;

/* I/O thread. */
static pthread_t io_thread;

/* Processing thread. */
static pthread_t proc_thread;

/* Scheduling context for processing thread. */
static ucontext_t proc_uctx_schedule;

/* Running context for processing thread. */
static ucontext_t proc_uctx_running;

/* Current process of processing thread. */
//static __thread ProcessBase *proc_process = NULL;
static ProcessBase *proc_process = NULL;

/* Flag indicating if performing safe call into legacy. */
// static __thread bool legacy = false;
static bool legacy = false;

/* Thunk to safely call into legacy. */
// static __thread function<void (void)> *legacy_thunk;
static const function<void (void)> *legacy_thunk;

/* Scheduler gate. */
static Gate *gate = new Gate();

/* Stack of recycled stacks. */
static stack<void *> *stacks = new stack<void *>();
static synchronizable(stacks) = SYNCHRONIZED_INITIALIZER;

/* Last exited process's stack to be recycled (global variable hack!). */
static void *recyclable = NULL;

/**
 * Filter. Synchronized support for using the filterer needs to be
 * recursive incase a filterer wants to do anything fancy (which is
 * possible and likely given that filters will get used for testing).
*/
static Filter *filterer = NULL;
static synchronizable(filterer) = SYNCHRONIZED_INITIALIZER_RECURSIVE;

/* Global garbage collector. */
PID<GarbageCollector> gc;


int set_nbio(int fd)
{
  int flags;

  /* If they have O_NONBLOCK, use the Posix way to do it. */
#ifdef O_NONBLOCK
  /* TODO(*): O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5. */
  if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
    flags = 0;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
  /* Otherwise, use the old way of doing it. */
  flags = 1;
  return ioctl(fd, FIOBIO, &flags);
#endif
}


Message* encode(const UPID &from, const UPID &to, const string &name, const string &data = "")
{
  Message* message = new Message();
  message->from = from;
  message->to = to;
  message->name = name;
  message->body = data;
  return message;
}


void transport(Message* message, ProcessBase* sender = NULL)
{
  if (message->to.ip == ip && message->to.port == port) {
    // Local message.
    process_manager->deliver(message, sender);
  } else {
    // Remote message.
    socket_manager->send(message);
  }
}


Message* parse(HttpRequest* request)
{
  if (request->method == "POST" && request->headers.count("User-Agent") > 0) {
    const string& temp = request->headers["User-Agent"];
    const string& libprocess = "libprocess/";
    size_t index = temp.find(libprocess);
    if (index != string::npos) {
      // Okay, now determine 'from'.
      const UPID from(temp.substr(index + libprocess.size(), temp.size()));

      // Now determine 'to'.
      index = request->path.find('/', 1);
      index = index != string::npos ? index - 1 : string::npos;
      const UPID to(request->path.substr(1, index), ip, port);

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
  }

  return NULL;
}


void handle_async(struct ev_loop *loop, ev_async *_, int revents)
{
  synchronized (watchers) {
    /* Start all the new I/O watchers. */
    while (!watchers->empty()) {
      ev_io *watcher = watchers->front();
      watchers->pop();
      ev_io_start(loop, watcher);
    }
  }

  synchronized (timeouts) {
    if (update_timer) {
      if (!timeouts->empty()) {
	// Determine the current time.
	ev_tstamp current_tstamp;
	if (clk != NULL) {
	  current_tstamp = clk->getCurrent();
	} else {
	  // TODO(benh): Unclear if want ev_now(...) or ev_time().
	  current_tstamp = ev_time();
	}

	timeouts_watcher.repeat = timeouts->begin()->first - current_tstamp;

	// Check when the timer event should fire.
        if (timeouts_watcher.repeat <= 0) {
	  // Feed the event now!
	  timeouts_watcher.repeat = 0;
	  ev_timer_again(loop, &timeouts_watcher);
          ev_feed_event(loop, &timeouts_watcher, EV_TIMEOUT);
        } else {
	  // Only repeat the timer if not using a manual clock (a call
	  // to Clock::advance() will force a timer event later).
	  if (clk != NULL && timeouts_watcher.repeat > 0)
	    timeouts_watcher.repeat = 0;
	  ev_timer_again(loop, &timeouts_watcher);
	}
      }

      update_timer = false;
    }
  }
}


void handle_timeout(struct ev_loop *loop, ev_timer *watcher, int revents)
{
  list<timeout> timedout;

  synchronized (timeouts) {
    ev_tstamp current_tstamp;

    if (clk != NULL) {
      current_tstamp = clk->getCurrent();
    } else {
      // TODO(benh): Unclear if want ev_now(...) or ev_time().
      current_tstamp = ev_time();
    }

    foreachpair (ev_tstamp tstamp, const list<timeout> &timedouts, *timeouts) {
      if (tstamp > current_tstamp)
        break;

      foreach (const timeout &timeout, timedouts) {
        if (clk != NULL) {
          // Update current time of process (if it's still
          // valid). Note that current time may be greater than the
          // timeout if a local message was received (and
          // happens-before kicks in), hence we use max.
          if (ProcessReference process = process_manager->use(timeout.pid)) {
            clk->setCurrent(process, max(clk->getCurrent(process),
                                         timeout.tstamp));
          }
        }
        // TODO(benh): Ensure deterministic order for testing?
        timedout.push_back(timeout);
      }
    }

    // Now erase the range of time stamps that timed out.
    timeouts->erase(timeouts->begin(), timeouts->upper_bound(current_tstamp));

    // Okay, so the time stamp for the next timeout should not have fired.
    CHECK(timeouts->empty() || (timeouts->begin()->first > current_tstamp));

    // Update the timer as necessary.
    // TODO(benh): Make this code look like the code in handle_async.
    if (!timeouts->empty() && clk == NULL) {
      timeouts_watcher.repeat = timeouts->begin()->first - current_tstamp;
      CHECK(timeouts_watcher.repeat > 0);
      ev_timer_again(loop, &timeouts_watcher);
    } else {
      timeouts_watcher.repeat = 0;
      ev_timer_again(loop, &timeouts_watcher);
    }

    update_timer = false;
  }

  foreach (const timeout &timeout, timedout) {
    process_manager->timedout(timeout.pid, timeout.generation);
  }
}


void handle_poll(struct ev_loop *loop, ev_io *watcher, int revents)
{
  tuple<UPID, int> *t = (tuple<UPID, int> *) watcher->data;
  process_manager->polled(t->get<0>(), t->get<1>());
  ev_io_stop(loop, watcher);
  delete watcher;
  delete t;
}


void recv_data(struct ev_loop *loop, ev_io *watcher, int revents)
{
  DataDecoder* decoder = (DataDecoder*) watcher->data;
  
  int c = watcher->fd;

  while (true) {
    const ssize_t size = 80 * 1024;
    ssize_t length = 0;

    char data[size];

    length = recv(c, data, size, 0);

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
        VLOG(2) << "Socket error while receiving: " << error;
      } else {
        VLOG(2) << "Socket closed while receiving";
      }
      socket_manager->closed(c);
      delete decoder;
      ev_io_stop(loop, watcher);
      delete watcher;
      break;
    } else {
      CHECK(length > 0);

      // Decode as much of the data as possible into HTTP requests.
      const deque<HttpRequest*>& requests = decoder->decode(data, length);

      if (!requests.empty()) {
        foreach (HttpRequest* request, requests) {
          process_manager->deliver(c, request);
        }
      } else if (requests.empty() && decoder->failed()) {
        VLOG(2) << "Decoder error while receiving";
        socket_manager->closed(c);
        delete decoder;
        ev_io_stop(loop, watcher);
        delete watcher;
        break;
      }
    }
  }
}


void send_data(struct ev_loop *loop, ev_io *watcher, int revents)
{
  DataEncoder* encoder = (DataEncoder*) watcher->data;

  int c = watcher->fd;

  while (true) {
    const void* data;
    size_t size;

    data = encoder->next(&size);
    CHECK(size > 0);

    ssize_t length = send(c, data, size, MSG_NOSIGNAL);

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
        VLOG(2) << "Socket error while sending: " << error;
      } else {
        VLOG(2) << "Socket closed while sending";
      }
      socket_manager->closed(c);
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

        // Check for more stuff to send on socket.
        encoder = socket_manager->next(c);
        if (encoder != NULL) {
          watcher->data = encoder;
        } else {
          // Nothing more to send right now, clean up.
          ev_io_stop(loop, watcher);
          delete watcher;
          break;
        }
      }
    }
  }
}


void sending_connect(struct ev_loop *loop, ev_io *watcher, int revents)
{
  int c = watcher->fd;

  // Now check that a successful connection was made.
  int opt;
  socklen_t optlen = sizeof(opt);

  if (getsockopt(c, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0 || opt != 0) {
    // Connect failure.
    VLOG(1) << "Socket error while connecting";
    socket_manager->closed(c);
    MessageEncoder* encoder = (MessageEncoder*) watcher->data;
    delete encoder;
    ev_io_stop(loop, watcher);
    delete watcher;
  } else {
    // We're connected! Now let's do some sending.
    ev_io_stop(loop, watcher);
    ev_io_init(watcher, send_data, c, EV_WRITE);
    ev_io_start(loop, watcher);
  }
}


void receiving_connect(struct ev_loop *loop, ev_io *watcher, int revents)
{
  int c = watcher->fd;

  // Now check that a successful connection was made.
  int opt;
  socklen_t optlen = sizeof(opt);

  if (getsockopt(c, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0 || opt != 0) {
    // Connect failure.
    VLOG(1) << "Socket error while connecting";
    socket_manager->closed(c);
    DataDecoder* decoder = (DataDecoder*) watcher->data;
    delete decoder;
    ev_io_stop(loop, watcher);
    delete watcher;
  } else {
    // We're connected! Now let's do some receiving.
    ev_io_stop(loop, watcher);
    ev_io_init(watcher, recv_data, c, EV_READ);
    ev_io_start(loop, watcher);
  }
}


void accept(struct ev_loop *loop, ev_io *watcher, int revents)
{
  int s = watcher->fd;

  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  int c = ::accept(s, (sockaddr *) &addr, &addrlen);

  if (c < 0) {
    return;
  }

  if (set_nbio(c) < 0) {
    close(c);
    return;
  }

  // Turn off Nagle (via TCP_NODELAY) so pipelined requests don't wait.
  int on = 1;
  if (setsockopt(c, SOL_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    close(c);
  } else {
    // Allocate and initialize the decoder and watcher.
    DataDecoder* decoder = new DataDecoder();

    ev_io *watcher = new ev_io();
    watcher->data = decoder;

    ev_io_init(watcher, recv_data, c, EV_READ);
    ev_io_start(loop, watcher);
  }
}


void * serve(void *arg)
{
  ev_loop(((struct ev_loop *) arg), 0);

  return NULL;
}


void trampoline(int stack0, int stack1, int process0, int process1)
{
  /* Unpackage the arguments. */
#ifdef __x86_64__
  CHECK (sizeof(unsigned long) == sizeof(ProcessBase *));
  void *stack = (void *)
    (((unsigned long) stack1 << 32) + (unsigned int) stack0);
  ProcessBase *process = (ProcessBase *)
    (((unsigned long) process1 << 32) + (unsigned int) process0);
#else
  CHECK (sizeof(unsigned int) == sizeof(ProcessBase *));
  void *stack = (void *) (unsigned int) stack0;
  ProcessBase *process = (ProcessBase *) (unsigned int) process0;
#endif /* __x86_64__ */

  /* Run the process. */
  process_manager->run(process);

  /* Prepare to recycle this stack (global variable hack!). */
  CHECK(recyclable == NULL);
  recyclable = stack;

  proc_process = NULL;
  setcontext(&proc_uctx_schedule);
}


void * schedule(void *arg)
{
  // Context for the entry into the schedule routine, used when a
  // process exits, so that other processes can get scheduled!
  if (getcontext(&proc_uctx_schedule) < 0)
    fatalerror("getcontext failed (schedule)");

  // Recycle the stack from an exited process.
  if (recyclable != NULL) {
    synchronized (stacks) {
      stacks->push(recyclable);
    }
    recyclable = NULL;
  }

  do {
    ProcessBase *process = process_manager->dequeue();

    if (process == NULL) {
      Gate::state_t old = gate->approach();
      process = process_manager->dequeue();
      if (process == NULL) {

        // When using the manual clock, we want to let all the
        // processes "run" up to the current time so that processes
        // receive messages in order. If we let one process have a
        // drastically advanced current time then it may try send
        // messages to another process that, due to the happens-before
        // relationship, will inherit it's drastically advanced
        // current time. If the processing thread gets to this point
        // (i.e., the point where no other processes are runnable)
        // with the manual clock means that all of the processes have
        // been run which could be run up to the current time. The
        // only way another process could become runnable is if (1) it
        // receives a message from another node, (2) a file descriptor
        // it is polling has become ready, or (3) if it has a
        // timeout. We can ignore processes that become runnable due
        // to receiving a message from another node or getting an
        // event on a file descriptor because that should not change
        // the timing happens-before relationship of the local
        // processes (unless of course the file descriptor was created
        // from something like timerfd, in which case, since the
        // programmer is not using the timing source provided in
        // libprocess and all bets are off). Thus, we can check that
        // there are no pending timeouts before the current time and
        // move the current time to the next timeout value, and tell
        // the timer to update itself.

        synchronized (timeouts) {
          if (clk != NULL) {
            if (!timeouts->empty()) {
              // Adjust the current time to the next timeout, provided
              // it is not past the elapsed time.
              ev_tstamp tstamp = timeouts->begin()->first;
              if (tstamp <= clk->getElapsed())
                clk->setCurrent(tstamp);
              
              update_timer = true;
              ev_async_send(loop, &async_watcher);
            } else {
              // Woah! This comment is the only thing in this else
              // branch because this is a pretty serious state ... the
              // only way to make progress is for another node to send
              // a message or for an event to occur on a file
              // descriptor that a process is polling. We may want to
              // consider doing (or printing) something here.
            }
          }
        }

	/* Wait at gate if idle. */
	gate->arrive(old);
	continue;
      } else {
	gate->leave();
      }
    }

    process->lock();
    {
      CHECK(process->state == ProcessBase::INIT ||
	    process->state == ProcessBase::READY ||
	    process->state == ProcessBase::INTERRUPTED ||
	    process->state == ProcessBase::TIMEDOUT);

      /* Continue process. */
      CHECK(proc_process == NULL);
      proc_process = process;
      swapcontext(&proc_uctx_running, &process->uctx);
      while (legacy) {
	(*legacy_thunk)();
	swapcontext(&proc_uctx_running, &process->uctx);
      }
      CHECK(proc_process != NULL);
      proc_process = NULL;
    }
    process->unlock();
  } while (true);
}


/*
 * We might find value in catching terminating signals at some point.
 * However, for now, adding signal handlers freely is not allowed
 * because they will clash with Java and Python virtual machines and
 * causes hard to debug crashes/segfaults.
 */


// void sigbad(int signal, struct sigcontext *ctx)
// {
//   /* Pass on the signal (so that a core file is produced).  */
//   struct sigaction sa;
//   sa.sa_handler = SIG_DFL;
//   sigemptyset(&sa.sa_mask);
//   sa.sa_flags = 0;
//   sigaction(signal, &sa, NULL);
//   raise(signal);
// }


void initialize(bool initialize_google_logging)
{
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

  if (initialize_google_logging) {
    google::InitGoogleLogging("libprocess");
    google::LogToStderr();
  }

//   /* Install signal handler. */
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
  process_manager = new ProcessManager();
  socket_manager = new SocketManager();

  // Setup processing thread.
  if (pthread_create (&proc_thread, NULL, schedule, NULL) != 0) {
    PLOG(FATAL) << "Failed to initialize, pthread_create";
  }

  ip = 0;
  port = 0;

  char *value;

  // Check environment for ip.
  value = getenv("LIBPROCESS_IP");
  if (value != NULL) {
    int result = inet_pton(AF_INET, value, &ip);
    if (result == 0) {
      fatal("LIBPROCESS_IP=%s was unparseable", value);
    } else if (result < 0) {
      fatalerror("failed to initialize (inet_pton)");
    }
  }

  // Check environment for port.
  value = getenv("LIBPROCESS_PORT");
  if (value != NULL) {
    int result = atoi(value);
    if (result < 0 || result > USHRT_MAX) {
      fatal("LIBPROCESS_PORT=%s is not a valid port", value);
    }
    port = result;
  }

  // Create a "server" socket for communicating with other nodes.
  if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0) {
    PLOG(FATAL) << "Failed to initialize, socket";
  }

  // Make socket non-blocking.
  if (set_nbio(s) < 0) {
    PLOG(FATAL) << "Failed to initialize, set_nbio";
  }

  // Allow address reuse.
  int on = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    PLOG(FATAL) << "Failed to initialize, setsockopt(SO_REUSEADDR)";
  }

  // Set up socket.
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = PF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    PLOG(FATAL) << "Failed to initialize, bind";
  }

  // Lookup and store assigned ip and assigned port.
  socklen_t addrlen = sizeof(addr);
  if (getsockname(s, (struct sockaddr *) &addr, &addrlen) < 0) {
    PLOG(FATAL) << "Failed to initialize, getsockname";
  }

  ip = addr.sin_addr.s_addr;
  port = ntohs(addr.sin_port);

  // Lookup hostname if missing ip or if ip is 127.0.0.1 in case we
  // actually have a valid external ip address. Note that we need only
  // one ip address, so that other processes can send and receive and
  // don't get confused as to whom they are sending to.
  if (ip == 0 || ip == 2130706433) {
    char hostname[512];

    if (gethostname(hostname, sizeof(hostname)) < 0) {
      PLOG(FATAL) << "Ffailed to initialize, gethostname";
    }

    // Lookup IP address of local hostname.
    struct hostent* he;

    if ((he = gethostbyname2(hostname, AF_INET)) == NULL) {
      PLOG(FATAL) << "Failed to initialize, gethostbyname2";
    }

    ip = *((uint32_t *) he->h_addr_list[0]);
  }

  if (listen(s, 500000) < 0) {
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

  ev_timer_init(&timeouts_watcher, handle_timeout, 0., 2100000.0);
  ev_timer_again(loop, &timeouts_watcher);

  ev_io_init(&server_watcher, accept, s, EV_READ);
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

  if (pthread_create(&io_thread, NULL, serve, loop) != 0) {
    PLOG(FATAL) << "Failed to initialize, pthread_create";
  }

  // Need to set initialzing here so that we can actually invoke
  // 'spawn' below for the garbage collector.
  initializing = false;

  // Create global garbage collector.
  gc = spawn(new GarbageCollector());

  char temp[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, (in_addr *) &ip, temp, INET_ADDRSTRLEN) == NULL) {
    PLOG(FATAL) << "Failed to initialize, inet_ntop";
  }

  VLOG(1) << "libprocess is initialized on " << temp << ":" << port;
}


HttpResponseWaiter::HttpResponseWaiter(const PID<HttpProxy>& _proxy)
  : proxy(_proxy) {}


HttpResponseWaiter::~HttpResponseWaiter() {}


void HttpResponseWaiter::await(const Future<HttpResponse>& future, bool persist)
{
  if (future.await(30)) {
    dispatch(proxy, &HttpProxy::ready, future, persist);  
  } else {
    dispatch(proxy, &HttpProxy::unavailable, persist);  
  }
}


HttpProxy::HttpProxy(int _c) : c(_c)
{
  // Create our waiter.
  waiter = new HttpResponseWaiter(self());
  spawn(waiter);
}


HttpProxy::~HttpProxy()
{
  send(waiter->self(), TERMINATE);
  wait(waiter->self());
  delete waiter;
}


void HttpProxy::handle(const Future<HttpResponse>& future, bool persist)
{
  dispatch(waiter, &HttpResponseWaiter::await, future, persist);
}


void HttpProxy::ready(const Future<HttpResponse>& future, bool persist)
{
  CHECK(future.ready());

  const HttpResponse& response = future.get();

  // Don't persist the connection if the responder doesn't want it to.
  if (response.headers.count("Connection") > 0) {
    const string& connection = response.headers.find("Connection")->second;
    if (connection == "close") {
      persist = false;
    }
  }

  // See the semantics of SocketManager::send for details about how
  // the socket will get closed (it might actually already be closed
  // before we issue this send).
  socket_manager->send(new HttpResponseEncoder(response), c, persist);
}


void HttpProxy::unavailable(bool persist)
{
  HttpResponse response = HttpServiceUnavailableResponse();

  // As above, the socket might all ready be closed when we do a send.
  socket_manager->send(new HttpResponseEncoder(response), c, persist);
}


SocketManager::SocketManager()
{
  synchronizer(this) = SYNCHRONIZED_INITIALIZER_RECURSIVE;
}


SocketManager::~SocketManager() {}


void SocketManager::link(ProcessBase *process, const UPID &to)
{
  // TODO(benh): The semantics we want to support for link are such
  // that if there is nobody to link to (local or remote) then a
  // EXITED message gets generated. This functionality has only
  // been implemented when the link is local, not remote. Of course,
  // if there is nobody listening on the remote side, then this should
  // work remotely ... but if there is someone listening remotely just
  // not at that id, then it will silently continue executing.

  CHECK(process != NULL);

  Node node(to.ip, to.port);

  synchronized (this) {
    // Check if node is remote and there isn't a persistant link.
    if ((node.ip != ip || node.port != port) && persists.count(node) == 0) {
      // Okay, no link, lets create a socket.
      int s;

      if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0) {
        fatalerror("failed to link (socket)");
      }

      if (set_nbio(s) < 0) {
        fatalerror("failed to link (set_nbio)");
      }

      sockets[s] = node;

      persists[node] = s;

      sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = PF_INET;
      addr.sin_port = htons(to.port);
      addr.sin_addr.s_addr = to.ip;

      // Allocate and initialize the decoder and watcher.
      DataDecoder* decoder = new DataDecoder();

      ev_io *watcher = new ev_io();
      watcher->data = decoder;

      // Try and connect to the node using this socket.
      if (connect(s, (sockaddr *) &addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
          fatalerror("failed to link (connect)");
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
  synchronized (this) {
    if (sockets.count(s) > 0) {
      CHECK(proxies.count(s) > 0);
      return proxies[s]->self();
    } else {
      // Register the socket with the manager for sending purposes. The
      // current design doesn't let us create a valid "node" for this
      // socket, so we use a "default" one for now.
      sockets[s] = Node();

      CHECK(proxies.count(s) == 0);

      HttpProxy* proxy = new HttpProxy(s);
      spawn(proxy, true);
      proxies[s] = proxy;
      return proxy->self();
    }
  }
}


void SocketManager::send(DataEncoder* encoder, int s, bool persist)
{
  CHECK(encoder != NULL);

  synchronized (this) {
    if (sockets.count(s) > 0) {
      if (outgoing.count(s) > 0) {
        outgoing[s].push(encoder);
      } else {
        // Initialize the outgoing queue.
        outgoing[s];

        // Allocate and initialize the watcher.
        ev_io *watcher = new ev_io();
        watcher->data = encoder;

        ev_io_init(watcher, send_data, s, EV_WRITE);

        synchronized (watchers) {
          watchers->push(watcher);
        }

        ev_async_send(loop, &async_watcher);
      }

      // Set the socket to get closed if not persistant.
      if (!persist) {
        disposables.insert(s);
      }
    } else {
      VLOG(1) << "Attempting to send on a no longer valid socket!";
    }
  }
}


void SocketManager::send(Message* message)
{
  CHECK(message != NULL);

  DataEncoder* encoder = new MessageEncoder(message);

  Node node(message->to.ip, message->to.port);

  synchronized (this) {
    // Check if there is already a socket.
    bool persistant = persists.count(node) > 0;
    bool temporary = temps.count(node) > 0;
    if (persistant || temporary) {
      int s = persistant ? persists[node] : temps[node];
      send(encoder, s, persistant);
    } else {
      // No peristant or temporary socket to the node currently
      // exists, so we create a temporary one.
      int s;

      if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0) {
        fatalerror("failed to send (socket)");
      }

      if (set_nbio(s) < 0) {
        fatalerror("failed to send (set_nbio)");
      }

      sockets[s] = node;

      temps[node] = s;
      disposables.insert(s);

      // Initialize the outgoing queue.
      outgoing[s];

      // Try and connect to the node using this socket.
      sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = PF_INET;
      addr.sin_port = htons(message->to.port);
      addr.sin_addr.s_addr = message->to.ip;

      // Allocate and initialize the watcher.
      ev_io *watcher = new ev_io();
      watcher->data = encoder;
    
      if (connect(s, (sockaddr *) &addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
          fatalerror("failed to send (connect)");
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


DataEncoder* SocketManager::next(int s)
{
  DataEncoder* encoder = NULL;

  synchronized (this) {
    CHECK(sockets.count(s) > 0);
    CHECK(outgoing.count(s) > 0);

    if (!outgoing[s].empty()) {
      // More messages!
      encoder = outgoing[s].front();
      outgoing[s].pop();
    } else {
      // No more messages ... erase the outgoing queue.
      outgoing.erase(s);

      // Close the socket if it was set for disposal.
      if (disposables.count(s) > 0) {
        // Also try and remove from temps.
        const Node& node = sockets[s];
        if (temps.count(node) > 0 && temps[node] == s) {
          temps.erase(node);
        } else if (proxies.count(s) > 0) {
          HttpProxy* proxy = proxies[s];
          proxies.erase(s);
          post(proxy->self(), TERMINATE);
        }

        disposables.erase(s);
        sockets.erase(s);
        close(s);
      }
    }
  }

  return encoder;
}


void SocketManager::closed(int s)
{
  synchronized (this) {
    if (sockets.count(s) > 0) {
      const Node& node = sockets[s];

      // Don't bother invoking exited unless socket was persistant.
      if (persists.count(node) > 0 && persists[node] == s) {
	persists.erase(node);
        exited(node);
      } else if (temps.count(node) > 0 && temps[node] == s) {
        temps.erase(node);
      } else if (proxies.count(s) > 0) {
        HttpProxy* proxy = proxies[s];
        proxies.erase(s);
        post(proxy->self(), TERMINATE);
      }

      outgoing.erase(s);
      disposables.erase(s);
      sockets.erase(s);
    }
  }

  // This might have just been a receiving socket (only sending
  // sockets, with the exception of the receiving side of a persistant
  // socket, get added to 'sockets'), so we want to make sure to call
  // close so that the file descriptor can get reused.
  close(s);
}


void SocketManager::exited(const Node &node)
{
  // TODO(benh): It would be cleaner if this routine could call back
  // into ProcessManager ... then we wouldn't have to convince
  // ourselves that the accesses to each Process object will always be
  // valid.
  synchronized (this) {
    list<UPID> removed;
    // Look up all linked processes.
    foreachpair (const UPID &pid, set<ProcessBase *> &processes, links) {
      if (pid.ip == node.ip && pid.port == node.port) {
        // N.B. If we call exited(pid) we might invalidate iteration.
        foreach (ProcessBase *process, processes) {
          Message* message = encode(pid, process->pid, EXITED);
          process->enqueue(message);
        }
        removed.push_back(pid);
      }
    }

    foreach (const UPID &pid, removed) {
      links.erase(pid);
    }
  }
}


void SocketManager::exited(ProcessBase *process)
{
  synchronized (this) {
    /* Remove any links this process might have had. */
    foreachpair (_, set<ProcessBase *> &processes, links) {
      processes.erase(process);
    }

    /* Look up all linked processes. */
    map<UPID, set<ProcessBase *> >::iterator it = links.find(process->pid);

    if (it != links.end()) {
      set<ProcessBase *> &processes = it->second;
      foreach (ProcessBase *p, processes) {
        CHECK(process != p);
        Message *message = encode(process->pid, p->pid, EXITED);
        // TODO(benh): Preserve happens-before when using clock.
        p->enqueue(message);
      }
      links.erase(process->pid);
    }
  }
}


ProcessManager::ProcessManager()
{
  synchronizer(processes) = SYNCHRONIZED_INITIALIZER;
  synchronizer(runq) = SYNCHRONIZED_INITIALIZER;
}


ProcessManager::~ProcessManager() {}


ProcessReference ProcessManager::use(const UPID &pid)
{
  if (pid.ip == ip && pid.port == port) {
    synchronized (processes) {
      if (processes.count(pid.id) > 0) {
        // Note that the ProcessReference constructor MUST get called
        // while holding the lock on processes.
        return ProcessReference(processes[pid.id]);
      }
    }
  }

  return ProcessReference(NULL);
}


bool ProcessManager::deliver(Message *message, ProcessBase *sender)
{
  CHECK(message != NULL);

  if (ProcessReference receiver = use(message->to)) {
    // If we have a local sender AND we are using a manual clock
    // then update the current time of the receiver to preserve
    // the happens-before relationship between the sender and
    // receiver. Note that the assumption is that the sender
    // remains valid for at least the duration of this routine (so
    // that we can look up it's current time).
    if (sender != NULL) {
      synchronized (timeouts) {
        if (clk != NULL) {
          ev_tstamp tstamp =
            max(clk->getCurrent(receiver), clk->getCurrent(sender));
          clk->setCurrent(receiver, tstamp);
        }
      }
    }

    VLOG(2) << "Delivering message name '" << message->name
            << "' to " << message->to << " from " << message->from;

    receiver->enqueue(message);
  } else {
    delete message;
    return false;
  }

  return true;
}


// TODO(benh): Refactor and share code with above!
bool ProcessManager::deliver(int c, HttpRequest *request, ProcessBase *sender)
{
  CHECK(request != NULL);

  // Determine whether or not this is a libprocess message.
  Message* message = parse(request);

  if (message != NULL) {
    delete request;
    return deliver(message, sender);
  }

  // Treat this as an HTTP request and check for a valid receiver.
  string temp = request->path.substr(1, request->path.find('/', 1) - 1);

  UPID to(temp, ip, port);

  if (ProcessReference receiver = use(to)) {
    // If we have a local sender AND we are using a manual clock
    // then update the current time of the receiver to preserve
    // the happens-before relationship between the sender and
    // receiver. Note that the assumption is that the sender
    // remains valid for at least the duration of this routine (so
    // that we can look up it's current time).
    if (sender != NULL) {
      synchronized (timeouts) {
        if (clk != NULL) {
          ev_tstamp tstamp =
            max(clk->getCurrent(receiver), clk->getCurrent(sender));
          clk->setCurrent(receiver, tstamp);
        }
      }
    }

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(c);
    
    // Create the promise to link with whatever gets returned.
    Promise<HttpResponse>* promise = new Promise<HttpResponse>();

    // Let the HttpProxy know about this request.
    dispatch(proxy, &HttpProxy::handle, promise->future(), request->keepAlive);

    VLOG(2) << "Delivering HTTP request for '" << request->path
            << "' to " << to;

    // Enqueue request and promise for receiver.
    receiver->enqueue(new pair<HttpRequest*, Promise<HttpResponse>*>(request, promise));
  } else {
    // This has no receiver, send error response.
    VLOG(1) << "Returning '404 Not Found' for HTTP request for '"
            << request->path << "'";

    // Get the HttpProxy pid for this socket.
    PID<HttpProxy> proxy = socket_manager->proxy(c);

    // Create a "future" response.
    Future<HttpResponse> future = HttpNotFoundResponse();

    // Let the HttpProxy know about this request.
    dispatch(proxy, &HttpProxy::handle, future, request->keepAlive);

    // Cleanup request.
    delete request;
    return false;
  }

  return true;
}


// TODO(benh): Refactor and share code with above!
bool ProcessManager::deliver(const UPID& to, function<void(ProcessBase*)>* dispatcher, ProcessBase *sender)
{
  CHECK(dispatcher != NULL);

  if (ProcessReference receiver = use(to)) {
    // If we have a local sender AND we are using a manual clock
    // then update the current time of the receiver to preserve
    // the happens-before relationship between the sender and
    // receiver. Note that the assumption is that the sender
    // remains valid for at least the duration of this routine (so
    // that we can look up it's current time).
    if (sender != NULL) {
      synchronized (timeouts) {
        if (clk != NULL) {
          ev_tstamp tstamp =
            max(clk->getCurrent(receiver), clk->getCurrent(sender));
          clk->setCurrent(receiver, tstamp);
        }
      }
    }

    VLOG(2) << "Delivering dispatcher to " << to;

    receiver->enqueue(dispatcher);
  } else {
    delete dispatcher;
    return false;
  }

  return true;
}


UPID ProcessManager::spawn(ProcessBase *process, bool manage)
{
  CHECK(process != NULL);

  process->state = ProcessBase::INIT;

  synchronized (processes) {
    if (processes.count(process->pid.id) > 0) {
      return UPID();
    } else {
      processes[process->pid.id] = process;
    }
  }

  void *stack = NULL;

  // Reuse a stack if any are available.
  synchronized (stacks) {
    if (!stacks->empty()) {
      stack = stacks->top();
      stacks->pop();
    }
  }

  if (stack == NULL) {
    const int protection = (PROT_READ | PROT_WRITE);
    const int flags = (MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT);

    stack = mmap(NULL, PROCESS_STACK_SIZE, protection, flags, -1, 0);

    if (stack == MAP_FAILED)
      fatalerror("mmap failed (spawn)");

    /* Disallow all memory access to the last page. */
    if (mprotect(stack, getpagesize(), PROT_NONE) != 0)
      fatalerror("mprotect failed (spawn)");
  }

  /* Set up the ucontext. */
  if (getcontext(&process->uctx) < 0)
    fatalerror("getcontext failed (spawn)");
    
  process->uctx.uc_stack.ss_sp = stack;
  process->uctx.uc_stack.ss_size = PROCESS_STACK_SIZE;
  process->uctx.uc_link = 0;

  /* Package the arguments. */
#ifdef __x86_64__
  CHECK(sizeof(unsigned long) == sizeof(ProcessBase *));
  int stack0 = (unsigned int) (unsigned long) stack;
  int stack1 = (unsigned long) stack >> 32;
  int process0 = (unsigned int) (unsigned long) process;
  int process1 = (unsigned long) process >> 32;
#else
  CHECK(sizeof(unsigned int) == sizeof(ProcessBase *));
  int stack0 = (unsigned int) stack;
  int stack1 = 0;
  int process0 = (unsigned int) process;
  int process1 = 0;
#endif /* __x86_64__ */

  makecontext(&process->uctx, (void (*)()) trampoline,
              4, stack0, stack1, process0, process1);

  /* Add process to the run queue. */
  enqueue(process);

  /* Use the garbage collector if requested. */
  if (manage) {
    dispatch(gc, &GarbageCollector::manage<ProcessBase>, process);
  }

  return process->self();
}



void ProcessManager::link(ProcessBase *process, const UPID &to)
{
  // Check if the pid is local.
  if (!(to.ip == ip && to.port == port)) {
    socket_manager->link(process, to);
  } else {
    // Since the pid is local we want to get a reference to it's
    // underlying process so that while we are invoking the link
    // manager we don't miss sending a possible EXITED.
    if (ProcessReference _ = use(to)) {
      socket_manager->link(process, to);
    } else {
      // Since the pid isn't valid it's process must have already died
      // (or hasn't been spawned yet) so send a process exit message.
      Message *message = encode(to, process->pid, EXITED);
      process->enqueue(message);
    }
  }
}


bool ProcessManager::receive(ProcessBase *process, double secs)
{
  CHECK(process != NULL);

  bool timedout = false;

  process->lock();
  {
    /* Ensure nothing enqueued since check in ProcessBase::receive. */
    if (process->messages.empty()) {
      if (secs > 0) {
        /* Create timeout. */
        const timeout &timeout = create_timeout(process, secs);

        /* Start the timeout. */
        start_timeout(timeout);

        /* Context switch. */
        process->state = ProcessBase::RECEIVING;
        swapcontext(&process->uctx, &proc_uctx_running);

        CHECK(process->state == ProcessBase::READY ||
	      process->state == ProcessBase::TIMEDOUT);

        /* Attempt to cancel the timer if necessary. */
        if (process->state != ProcessBase::TIMEDOUT) {
          cancel_timeout(timeout);
        } else {
          timedout = true;
        }

        /* N.B. No cancel means possible unnecessary timeouts. */

        process->state = ProcessBase::RUNNING;
      
        /* Update the generation (handles racing timeouts). */
        process->generation++;
      } else {
        /* Context switch. */
        process->state = ProcessBase::RECEIVING;
        swapcontext(&process->uctx, &proc_uctx_running);
        CHECK(process->state == ProcessBase::READY);
        process->state = ProcessBase::RUNNING;
      }
    }
  }
  process->unlock();

  return !timedout;
}


bool ProcessManager::serve(ProcessBase *process, double secs)
{
  CHECK(process != NULL);

  bool timedout = false;

  process->lock();
  {
    /* Ensure nothing enqueued since check in ProcessBase::serve. */
    if (process->messages.empty() &&
        process->requests.empty() &&
        process->dispatchers.empty()) {
      if (secs > 0) {
        /* Create timeout. */
        const timeout &timeout = create_timeout(process, secs);

        /* Start the timeout. */
        start_timeout(timeout);

        /* Context switch. */
        process->state = ProcessBase::SERVING;
        swapcontext(&process->uctx, &proc_uctx_running);

        CHECK(process->state == ProcessBase::READY ||
	      process->state == ProcessBase::TIMEDOUT);

        /* Attempt to cancel the timer if necessary. */
        if (process->state != ProcessBase::TIMEDOUT) {
          cancel_timeout(timeout);
        } else {
          timedout = true;
        }

        /* N.B. No cancel means possible unnecessary timeouts. */

        process->state = ProcessBase::RUNNING;
      
        /* Update the generation (handles racing timeouts). */
        process->generation++;
      } else {
        /* Context switch. */
        process->state = ProcessBase::SERVING;
        swapcontext(&process->uctx, &proc_uctx_running);
        CHECK(process->state == ProcessBase::READY);
        process->state = ProcessBase::RUNNING;
      }
    }
  }
  process->unlock();

  return !timedout;
}


void ProcessManager::pause(ProcessBase *process, double secs)
{
  CHECK(process != NULL);

  process->lock();
  {
    if (secs > 0) {
      /* Create/Start the timeout. */
      start_timeout(create_timeout(process, secs));

      /* Context switch. */
      process->state = ProcessBase::PAUSED;
      swapcontext(&process->uctx, &proc_uctx_running);
      CHECK(process->state == ProcessBase::TIMEDOUT);
      process->state = ProcessBase::RUNNING;
    } else {
      /* Modified context switch (basically a yield). */
      process->state = ProcessBase::READY;
      enqueue(process);
      swapcontext(&process->uctx, &proc_uctx_running);
      CHECK(process->state == ProcessBase::READY);
      process->state = ProcessBase::RUNNING;
    }
  }
  process->unlock();
}


void ProcessManager::terminate(const UPID& pid, bool inject, ProcessBase* sender)
{
  if (ProcessReference process = use(pid)) {
    if (sender != NULL) {
      synchronized (timeouts) {
        if (clk != NULL) {
          ev_tstamp tstamp =
            max(clk->getCurrent(process), clk->getCurrent(sender));
          clk->setCurrent(process, tstamp);
        }
      }

      process->enqueue(encode(sender->self(), pid, TERMINATE), inject);
    } else {
      process->enqueue(encode(UPID(), pid, TERMINATE), inject);
    }
  }
}


bool ProcessManager::wait(ProcessBase *process, const UPID &pid)
{
  bool waited = false;

  /* Now we can add the process to the waiters. */
  synchronized (processes) {
    if (processes.count(pid.id) > 0) {
      CHECK(processes[pid.id]->state != ProcessBase::FINISHED);
      waiters[processes[pid.id]].insert(process);
      waited = true;
    }
  }

  /* If we waited then we should context switch. */
  if (waited) {
    process->lock();
    {
      if (process->state == ProcessBase::RUNNING) {
        /* Context switch. */
        process->state = ProcessBase::WAITING;
        swapcontext(&process->uctx, &proc_uctx_running);
        CHECK(process->state == ProcessBase::READY);
        process->state = ProcessBase::RUNNING;
      } else {
        /* Process is cleaned up and we have been removed from waiters. */
        CHECK(process->state == ProcessBase::INTERRUPTED);
        process->state = ProcessBase::RUNNING;
      }
    }
    process->unlock();
  }

  return waited;
}


bool ProcessManager::external_wait(const UPID &pid)
{
  // We use a gate for external waiters. A gate is single use. That
  // is, a new gate is created when the first external thread shows
  // up and wants to wait for a process that currently has no
  // gate. Once that process exits, the last external thread to
  // leave the gate will also clean it up. Note that a gate will
  // never get more external threads waiting on it after it has been
  // opened, since the process should no longer be valid and
  // therefore will not have an entry in 'processes'.

  Gate *gate = NULL;
  Gate::state_t old;

  /* Try and approach the gate if necessary. */
  synchronized (processes) {
    if (processes.count(pid.id) > 0) {
      ProcessBase *process = processes[pid.id];
      CHECK(process->state != ProcessBase::FINISHED);

      /* Check and see if a gate already exists. */
      if (gates.find(process) == gates.end()) {
        gates[process] = new Gate();
      }

      gate = gates[process];
      old = gate->approach();
    }
  }

  /* Now arrive at the gate and wait until it opens. */
  if (gate != NULL) {
    gate->arrive(old);

    if (gate->empty()) {
      delete gate;
    }

    return true;
  }

  return false;
}


bool ProcessManager::poll(ProcessBase *process, int fd, int op, double secs, bool ignore)
{
  CHECK(process != NULL);

  bool interrupted = false;

  process->lock();
  {
    /* Consider a non-empty message queue as an immediate interrupt. */
    if (!ignore && !process->messages.empty()) {
      process->unlock();
      return false;
    }

    // Treat an poll with a bad fd as an interruptible pause!
    if (fd >= 0) {
      /* Allocate/Initialize the watcher. */
      ev_io *watcher = new ev_io();

      if ((op & ProcessBase::RDWR) == ProcessBase::RDWR) {
        ev_io_init(watcher, handle_poll, fd, EV_READ | EV_WRITE);
      } else if ((op & ProcessBase::RDONLY) == ProcessBase::RDONLY) {
        ev_io_init(watcher, handle_poll, fd, EV_READ);
      } else if ((op & ProcessBase::WRONLY) == ProcessBase::WRONLY) {
        ev_io_init(watcher, handle_poll, fd, EV_WRITE);
      }

      // Tuple describing state (on heap in case we can't "cancel" it,
      // the watcher will always fire, even if we get interrupted and
      // return early, so this tuple will get cleaned up when the
      // watcher runs).
      watcher->data = new tuple<UPID, int>(process->pid, process->generation);

      /* Enqueue the watcher. */
      synchronized (watchers) {
        watchers->push(watcher);
      }
    
      /* Interrupt the loop. */
      ev_async_send(loop, &async_watcher);
    }

    CHECK(secs >= 0);

    timeout timeout;

    if (secs != 0) {
      timeout = create_timeout(process, secs);
      start_timeout(timeout);
    }

    /* Context switch. */
    process->state = ProcessBase::POLLING;
    swapcontext(&process->uctx, &proc_uctx_running);
    CHECK(process->state == ProcessBase::READY ||
	  process->state == ProcessBase::TIMEDOUT ||
	  process->state == ProcessBase::INTERRUPTED);

    /* Attempt to cancel the timer if necessary. */
    if (secs != 0) {
      if (process->state != ProcessBase::TIMEDOUT) {
        cancel_timeout(timeout);
      }
    }

    if (process->state == ProcessBase::INTERRUPTED) {
      interrupted = true;
    }

    process->state = ProcessBase::RUNNING;
      
    /* Update the generation (handles racing polled). */
    process->generation++;
  }
  process->unlock();

  return !interrupted;
}


void ProcessManager::enqueue(ProcessBase *process)
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
    
  /* Wake up the processing thread if necessary. */
  gate->open();
}


ProcessBase * ProcessManager::dequeue()
{
  // TODO(benh): Remove a process from this thread's runq. If there
  // are no processes to run, and this is not a dedicated thread, then
  // steal one from another threads runq.

  ProcessBase *process = NULL;

  synchronized (runq) {
    if (!runq.empty()) {
      process = runq.front();
      runq.pop_front();
    }
  }

  return process;
}


void ProcessManager::timedout(const UPID &pid, int generation)
{
  if (ProcessReference process = use(pid)) {
    process->lock();
    {
      // We know we timed out if the state != READY after a timeout
      // but the generation is still the same.
      if (process->state != ProcessBase::READY &&
          process->generation == generation) {

        // The process could be in any of the following states,
        // including RUNNING if a pause, receive, or poll was
        // initiated by an "outside" thread (e.g., in the constructor
        // of the process).
        CHECK(process->state == ProcessBase::RUNNING ||
	      process->state == ProcessBase::RECEIVING ||
	      process->state == ProcessBase::SERVING ||
	      process->state == ProcessBase::POLLING ||
	      process->state == ProcessBase::INTERRUPTED ||
	      process->state == ProcessBase::PAUSED);

        if (process->state != ProcessBase::RUNNING ||
            process->state != ProcessBase::INTERRUPTED ||
            process->state != ProcessBase::FINISHING) {
          process_manager->enqueue(process);
        }

        // We always have a timeout override the state (unless we are
        // exiting). This includes overriding INTERRUPTED. This means
        // that a process that was polling when selected from the
        // runq will fall out because of a timeout even though it also
        // received a message.
        if (process->state != ProcessBase::FINISHING) {
          process->state = ProcessBase::TIMEDOUT;
        }
      }
    }
    process->unlock();
  }
}


void ProcessManager::polled(const UPID &pid, int generation)
{
  if (ProcessReference process = use(pid)) {
    process->lock();
    {
      if (process->state == ProcessBase::POLLING &&
          process->generation == generation) {
        process->state = ProcessBase::READY;
        enqueue(process);
      }
    }
    process->unlock();
  }
}


void ProcessManager::run(ProcessBase *process)
{
  // Each process gets locked before 'schedule' runs it to enforce
  // atomicity for the blocking routines (receive, poll, pause,
  // etc). So, we only need to unlock the process here.
  {
    process->state = ProcessBase::RUNNING;
  }
  process->unlock();

  try {
    (*process)();
  } catch (const std::exception &e) {
    std::cerr << "libprocess: " << process->pid
              << " exited due to "
              << e.what() << std::endl;
  } catch (...) {
    std::cerr << "libprocess: " << process->pid
              << " exited due to unknown exception" << std::endl;
  }

  cleanup(process);
}


void ProcessManager::cleanup(ProcessBase *process)
{
  // Processes that were waiting on exiting process.
  list<ProcessBase *> resumable;

  // Possible gate non-libprocess threads are waiting at.
  Gate *gate = NULL;

  // Stop new process references from being created.
  process->state = ProcessBase::FINISHING;

  /* Remove process. */
  synchronized (processes) {
    // Remove from internal clock (if necessary).
    synchronized (timeouts) {
      if (clk != NULL)
        clk->discard(process);
    }

    // Wait for all process references to get cleaned up.
    while (process->refs > 0) {
      asm ("pause");
      __sync_synchronize();
    }

    process->lock();
    {
      // Free any pending messages.
      while (!process->messages.empty()) {
        Message *message = process->messages.front();
        process->messages.pop_front();
        delete message;
      }

      // Free any pending requests.
      while (!process->requests.empty()) {
        pair<HttpRequest*, Promise<HttpResponse>*>* request = process->requests.front();
        process->requests.pop_front();
        delete request;
      }

      // Free any pending dispatchers.
      while (!process->dispatchers.empty()) {
        function<void(ProcessBase*)>* dispatcher = process->dispatchers.front();
        process->dispatchers.pop_front();
        delete dispatcher;
      }

      // Free current message.
      if (process->current) {
        delete process->current;
      }

      processes.erase(process->pid.id);

      // Confirm that the process is not in any waiting queue.
      foreachpair (_, set<ProcessBase *> &waiting, waiters) {
        CHECK(waiting.find(process) == waiting.end());
      }

      // Grab all the waiting processes that are now resumable.
      foreach (ProcessBase *waiter, waiters[process]) {
        resumable.push_back(waiter);
      }

      waiters.erase(process);

      // Lookup gate to wake up waiting non-libprocess threads.
      map<ProcessBase *, Gate *>::iterator it = gates.find(process);
      if (it != gates.end()) {
        gate = it->second;
        // N.B. The last thread that leaves the gate also free's it.
        gates.erase(it);
      }

      CHECK(process->refs == 0);
      process->state = ProcessBase::FINISHED;
    }
    process->unlock();
  }

  // Inform socket manager.
  socket_manager->exited(process);

  // Confirm process not in runq.
  synchronized (runq) {
    CHECK(find(runq.begin(), runq.end(), process) == runq.end());
  }

  // N.B. After opening the gate we can no longer dereference
  // 'process' since it might already be cleaned up by user code (a
  // waiter might have cleaned up the stack where the process was
  // allocated).
  if (gate != NULL) {
    gate->open();
  }

  // And resume all processes waiting too.
  foreach (ProcessBase *p, resumable) {
    p->lock();
    {
      // Process 'p' might be RUNNING because it is racing to become
      // WAITING while we are actually trying to get it to become
      // running again.
      // TODO(benh): Once we actually run multiple processes at a
      // time (using multiple threads) this logic will need to get
      // made thread safe (in particular, a process may be
      // FINISHING).
      CHECK(p->state == ProcessBase::RUNNING ||
	    p->state == ProcessBase::WAITING);
      if (p->state == ProcessBase::RUNNING) {
        p->state = ProcessBase::INTERRUPTED;
      } else {
        p->state = ProcessBase::READY;
        enqueue(p);
      }
    }
    p->unlock();
  }
}


void ProcessManager::invoke(const function<void (void)> &thunk)
{
  legacy_thunk = &thunk;
  legacy = true;
  CHECK(proc_process != NULL);
  swapcontext(&proc_process->uctx, &proc_uctx_running);
  legacy = false;
}


timeout ProcessManager::create_timeout(ProcessBase *process, double secs)
{
  CHECK(process != NULL);

  ev_tstamp tstamp;

  synchronized (timeouts) {
    if (clk != NULL) {
      tstamp = clk->getCurrent(process) + secs;
    } else {
      // TODO(benh): Unclear if want ev_now(...) or ev_time().
      tstamp = ev_time() + secs;
    }
  }

  timeout timeout;
  timeout.tstamp = tstamp;
  timeout.pid = process->pid;
  timeout.generation = process->generation;

  return timeout;
}


void ProcessManager::start_timeout(const timeout &timeout)
{
  /* Add the timer. */
  synchronized (timeouts) {
    if (timeouts->size() == 0 || timeout.tstamp < timeouts->begin()->first) {
      // Need to interrupt the loop to update/set timer repeat.
      (*timeouts)[timeout.tstamp].push_back(timeout);
      update_timer = true;
      ev_async_send(loop, &async_watcher);
    } else {
      // Timer repeat is adequate, just add the timeout.
      CHECK(timeouts->size() >= 1);
      (*timeouts)[timeout.tstamp].push_back(timeout);
    }
  }
}


void ProcessManager::cancel_timeout(const timeout &timeout)
{
  synchronized (timeouts) {
    // Check if the timeout is still pending, and if so, erase
    // it. In addition, erase an empty list if we just removed the
    // last timeout.
    if (timeouts->count(timeout.tstamp) > 0) {
      (*timeouts)[timeout.tstamp].remove(timeout);
      if ((*timeouts)[timeout.tstamp].empty())
        timeouts->erase(timeout.tstamp);
    }
  }
}


double Clock::elapsed()
{
  synchronized (timeouts) {
    if (clk != NULL) {
      return clk->getElapsed();
    } else {
      return ev_time();
    }
  }
}


void Clock::pause()
{
  initialize();

  synchronized (timeouts) {
    // For now, only one global clock (rather than clock per
    // process). This Means that we have to take special care to
    // ensure happens-before timing (currently done for local message
    // sends and spawning new processes, not currently done for
    // EXITED messages).
    if (clk == NULL) {
      clk = new InternalClock();

      // The existing libev timer might actually timeout, but now that
      // clk != NULL, no "time" will actually have passed, so no
      // timeouts will actually occur.
    }
  }
}


void Clock::resume()
{
  initialize();

  synchronized (timeouts) {
    if (clk != NULL) {
      delete clk;
      clk = NULL;
    }

    update_timer = true;
    ev_async_send(loop, &async_watcher);
  }
}


void Clock::advance(double secs)
{
  synchronized (timeouts) {
    if (clk != NULL) {
      clk->setElapsed(clk->getElapsed() + secs);

      // Might need to wakeup the processing thread.
      gate->open();
    }
  }
}


ProcessBase::ProcessBase(const std::string& _id)
{
  initialize();

  pthread_mutex_init(&m, NULL);

  refs = 0;
  current = NULL;
  generation = 0;

  // Generate string representation of unique id for process.
  if (_id != "") {
    pid.id = _id;
  } else {
    stringstream out;
    out << __sync_add_and_fetch(&id, 1);
    pid.id = out.str();
  }

  pid.ip = ip;
  pid.port = port;

  // If using a manual clock, try and set current time of process
  // using happens before relationship between creator and createe!
  synchronized (timeouts) {
    if (clk != NULL) {
      if (pthread_self() == proc_thread) {
        CHECK(proc_process != NULL);
        clk->setCurrent(this, clk->getCurrent(proc_process));
      } else {
        clk->setCurrent(this, clk->getCurrent());
      }
    }
  }
}


ProcessBase::~ProcessBase() {}


void ProcessBase::enqueue(Message* message, bool inject)
{
  CHECK(message != NULL);

  // TODO(benh): Put filter inside lock statement below so that we can
  // guarantee the order of the messages seen by a filter are the same
  // as the order of messages seen by the process.
  synchronized (filterer) {
    if (filterer != NULL) {
      if (filterer->filter(message)) {
        delete message;
        return;
      }
    }
  }

  UPID delegate;

  lock();
  {
    CHECK(state != FINISHED);

    // Check and see if we should delegate this message.
    if (delegates.count(message->name) > 0) {
      delegate = delegates[message->name];
    } else {
      if (!inject) {
        messages.push_back(message);
      } else {
        messages.push_front(message);
      }

      if (state == RECEIVING || state == SERVING) {
        state = READY;
        process_manager->enqueue(this);
      } else if (state == POLLING) {
        state = INTERRUPTED;
        process_manager->enqueue(this);
      }

      CHECK(state == INIT ||
            state == READY ||
            state == RUNNING ||
            state == PAUSED ||
            state == WAITING ||
            state == INTERRUPTED ||
            state == TIMEDOUT ||
            state == FINISHING);
    }
  }
  unlock();

  // Delegate this message if necessary.
  if (delegate != UPID()) {
    VLOG(1) << "Delegating message '" << message->name << "' to " << delegate;
    message->to = delegate;
    transport(message, this);
  }
}


void ProcessBase::enqueue(pair<HttpRequest*, Promise<HttpResponse>*>* request)
{
  CHECK(request != NULL);

  // TODO(benh): Support filtering HTTP requests.

  lock();
  {
    CHECK(state != FINISHED);

    requests.push_back(request);

    if (state == SERVING) {
      state = READY;
      process_manager->enqueue(this);
    } else if (state == POLLING) {
      state = INTERRUPTED;
      process_manager->enqueue(this);
    }

    CHECK(state == INIT ||
	  state == READY ||
	  state == RUNNING ||
	  state == RECEIVING ||
	  state == PAUSED ||
	  state == WAITING ||
	  state == INTERRUPTED ||
	  state == TIMEDOUT ||
	  state == FINISHING);
  }
  unlock();
}


void ProcessBase::enqueue(function<void(ProcessBase*)>* dispatcher)
{
  CHECK(dispatcher != NULL);

  // TODO(benh): Support filtering dispatches.

  lock();
  {
    CHECK(state != FINISHED);

    dispatchers.push_back(dispatcher);

    if (state == SERVING) {
      state = READY;
      process_manager->enqueue(this);
    } else if (state == POLLING) {
      state = INTERRUPTED;
      process_manager->enqueue(this);
    }

    CHECK(state == INIT ||
	  state == READY ||
	  state == RUNNING ||
	  state == RECEIVING ||
	  state == PAUSED ||
	  state == WAITING ||
	  state == INTERRUPTED ||
	  state == TIMEDOUT ||
	  state == FINISHING);
  }
  unlock();
}


template <>
Message * ProcessBase::dequeue()
{
  Message *message = NULL;

  lock();
  {
    CHECK(state == RUNNING);
    if (!messages.empty()) {
      message = messages.front();
      messages.pop_front();
    }
  }
  unlock();

  return message;
}


template <>
pair<HttpRequest*, Promise<HttpResponse>*>* ProcessBase::dequeue()
{
  pair<HttpRequest*, Promise<HttpResponse>*>* request = NULL;

  lock();
  {
    CHECK(state == RUNNING);
    if (!requests.empty()) {
      request = requests.front();
      requests.pop_front();
    }
  }
  unlock();

  return request;
}


template <>
function<void(ProcessBase*)> * ProcessBase::dequeue()
{
  function<void(ProcessBase*)> *dispatcher = NULL;

  lock();
  {
    CHECK(state == RUNNING);
    if (!dispatchers.empty()) {
      dispatcher = dispatchers.front();
      dispatchers.pop_front();
    }
  }
  unlock();

  return dispatcher;
}


void ProcessBase::inject(const UPID& from, const string& name, const char* data, size_t length)
{
  if (!from)
    return;

  enqueue(encode(from, pid, name, string(data, length)), true);
}


void ProcessBase::send(const UPID& to, const string& name, const char* data, size_t length)
{
  if (!to) {
    return;
  }

  // Encode and transport outgoing message.
  transport(encode(pid, to, name, string(data, length)), this);
}


string ProcessBase::receive(double secs)
{
  // Free current message.
  if (current != NULL) {
    delete current;
    current = NULL;
  }

  // Check if there is a message.
 check:
  if ((current = dequeue<Message>()) != NULL) {
    return name();
  }

  if (pthread_self() == proc_thread) {
    // Avoid blocking if negative seconds.
    if (secs >= 0) {
      if (!process_manager->receive(this, secs)) {
        goto timeout;
      } else {
        lock();
        {
          CHECK(!messages.empty());
        }
        unlock();
        goto check;
      }
    } else {
      goto timeout;
    }
  } else {
    // TODO(benh): Handle calling receive from an outside thread.
    fatal("unimplemented");
  }

 timeout:
  CHECK(current == NULL);
  current = encode(self(), pid, TIMEOUT);
  return TIMEOUT;
}


string ProcessBase::serve(double secs, bool once)
{
  double startTime = elapsedTime();

  do {
    // Free current message.
    if (current != NULL) {
      delete current;
      current = NULL;
    }

    // Check if there is a message, an HTTP request, or a dispatch.
  check:
    pair<HttpRequest*, Promise<HttpResponse>*>* request =
      dequeue<pair<HttpRequest*, Promise<HttpResponse>*> >();

    if (request != NULL) {
      size_t index = request->first->path.find('/', 1);
      index = index != string::npos ? index + 1 : request->first->path.size();
      const string& name = request->first->path.substr(index);
      if (httpHandlers.count(name) > 0) {
        Promise<HttpResponse> promise = httpHandlers[name](*request->first);
        internal::associate(promise, *request->second);
      } else {
        VLOG(1) << "Returning '404 Not Found' for HTTP request for '"
                << request->first->path << "'";
        request->second->set(HttpNotFoundResponse());
      }
      delete request->first;
      delete request->second;
      delete request;
      continue;
    }

    function<void(ProcessBase*)>* dispatcher =
      dequeue<function<void(ProcessBase*)> >();

    if (dispatcher != NULL) {
      (*dispatcher)(this);
      delete dispatcher;
      continue;
    }

    // We drain the message queue last because otherwise a bunch of
    // dispatches might get enqueued but then a single TERMINATE get's
    // enqueued and none of the dispatches get processed (even if they
    // got enqueued first). There needs to be a better way of having a
    // single queue holding something like a boost var type that we
    // can switch on to determine how to handle the next message.

//     struct Event
//     {
//       enum {
//         HTTP,
//         DISPATCH,
//         MESSAGE
//       } type;

//       pair<HttpRequest*, Future<HttpResponse>*>* http() {}
//       function<void(ProcessBase*)>* dispatch() {}
//       Message* message() {}

//       void* value;
//     };

//     switch (queue.head().type()) {
//       case HTTP:
//     }

//     if (queue.head().type() == HTTP) {

//     }

    if ((current = dequeue<Message>()) != NULL) {
      if (messageHandlers.count(name()) > 0) {
        messageHandlers[name()]();
	continue;
      } else {
        return name();
      }
    }

    // Okay, nothing found, possibly block in ProcessManager::serve.
    if (pthread_self() == proc_thread) {
      // Avoid blocking if seconds has elapsed.
      if (secs > 0) {
        secs = secs - (elapsedTime() - startTime);
        if (secs <= 0) {
          secs = -1;
        }
      }

      // Avoid blocking if negative seconds.
      if (secs >= 0) {
        if (!process_manager->serve(this, secs)) {
          goto timeout;
        } else {
          // Okay, something should be ready.
          lock();
          {
            CHECK(!messages.empty() ||
		  !requests.empty() ||
		  !dispatchers.empty());
          }
          unlock();
          goto check;
        }
      } else {
        goto timeout;
      }
    } else {
      // TODO(benh): Handle calling serve from an outside thread.
      fatal("unimplemented");
    }
  } while (!once);

  return NOTHING;

 timeout:
  CHECK(current == NULL);
  current = encode(self(), pid, TIMEOUT);
  return TIMEOUT;
}


UPID ProcessBase::from() const
{
  if (current != NULL) {
    return current->from;
  } else {
    return UPID();
  }
}


const string& ProcessBase::name() const
{
  if (current != NULL) {
    return current->name;
  } else {
    return NOTHING;
  }
}


const std::string& ProcessBase::body() const
{
  static const string EMPTY;

  if (current != NULL) {
    return current->body;
  }

  return EMPTY;
}


void ProcessBase::pause(double secs)
{
  if (pthread_self() == proc_thread) {
    process_manager->pause(this, secs);
  } else {
    sleep(secs);
  }
}


UPID ProcessBase::link(const UPID& to)
{
  if (!to) {
    return to;
  }

  process_manager->link(this, to);

  return to;
}


bool ProcessBase::poll(int fd, int op, double secs, bool ignore)
{
  if (secs < 0) {
    return true;
  }

  /* TODO(benh): Handle invoking poll from "outside" thread. */
  if (pthread_self() != proc_thread) {
    fatal("unimplemented");
  }

  return process_manager->poll(this, fd, op, secs, ignore);
}


bool ProcessBase::ready(int fd, int op)
{
  if (fd < 0) {
    return false;
  }

  fd_set rdset;
  fd_set wrset;

  FD_ZERO(&rdset);
  FD_ZERO(&wrset);

  if (op & RDWR) {
    FD_SET(fd, &rdset);
    FD_SET(fd, &wrset);
  } else if (op & RDONLY) {
    FD_SET(fd, &rdset);
  } else if (op & WRONLY) {
    FD_SET(fd, &wrset);
  }

  timeval timeout;
  memset(&timeout, 0, sizeof(timeout));

  select(fd + 1, &rdset, &wrset, NULL, &timeout);

  return FD_ISSET(fd, &rdset) || FD_ISSET(fd, &wrset);
}


double ProcessBase::elapsedTime()
{
  double now = 0;

  synchronized (timeouts) {
    if (clk != NULL) {
      now = clk->getCurrent(this);
    } else {
      // TODO(benh): Unclear if want ev_now(...) or ev_time().
      now = ev_time();
    }
  }

  return now;
}


UPID ProcessBase::spawn(ProcessBase* process, bool manage)
{
  initialize();

  if (process != NULL) {
    // If using a manual clock, try and set current time of process
    // using happens before relationship between spawner and spawnee!
    synchronized (timeouts) {
      if (clk != NULL) {
        if (pthread_self() == proc_thread) {
          CHECK(proc_process != NULL);
          clk->setCurrent(process, clk->getCurrent(proc_process));
        } else {
          clk->setCurrent(process, clk->getCurrent());
        }
      }
    }

    VLOG(1) << "Spawning process " << process->self();

    return process_manager->spawn(process, manage);
  } else {
    return UPID();
  }
}


void terminate(const UPID& pid, bool inject)
{
  if (pthread_self() != proc_thread) {
    process_manager->terminate(pid, inject);
  } else {
    process_manager->terminate(pid, inject, proc_process);
  }
}


class WaitWaiter : public Process<WaitWaiter>
{
public:
  WaitWaiter(const UPID& _pid, double _secs, bool* _waited)
    : pid(_pid), secs(_secs), waited(_waited) {}

protected:
  virtual void operator () ()
  {
    link(pid);
    receive(secs);
    if (name() == EXITED) {
      *waited = true;
    } else {
      *waited = false;
    }
  }

private:
  const UPID pid;
  const double secs;
  bool* waited;
};


bool wait(const UPID& pid, double secs)
{
  initialize();

  if (!pid) {
    return false;
  }

  if (secs == 0) {
    // N.B. This could result in a deadlock! We could check if such
    // was the case by doing:
    //
    //   if (proc_process && proc_process->pid == pid) {
    //     handle deadlock here;
    //   }
    //
    // But for now, deadlocks seem like better bugs to try and fix
    // than segmentation faults that might occur because a client
    // thinks it has waited on a process and it is now finished (and
    // can be cleaned up).

    if (pthread_self() != proc_thread) {
      return process_manager->external_wait(pid);
    }

    return process_manager->wait(proc_process, pid);
  }

  bool waited = false;

  WaitWaiter waiter(pid, secs, &waited);
  wait(spawn(&waiter));

  return waited;
}


void invoke(const function<void (void)> &thunk)
{
  initialize();
  ProcessManager::invoke(thunk);
}


void filter(Filter *filter)
{
  initialize();

  synchronized (filterer) {
    filterer = filter;
  }
}


void post(const UPID& to, const string& name, const char* data, size_t length)
{
  initialize();

  if (!to) {
    return;
  }

  // Encode and transport outgoing message.
  transport(encode(UPID(), to, name, string(data, length)));
}


namespace internal {

void dispatch(const UPID& pid, function<void(ProcessBase*)>* dispatcher)
{
  initialize();

  if (pthread_self() != proc_thread) {
    process_manager->deliver(pid, dispatcher);
  } else {
    process_manager->deliver(pid, dispatcher, proc_process);
  }
}

}}  // namespace process { namespace internal {
