#include <assert.h>
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

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <boost/tuple/tuple.hpp>

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

#include "config.hpp"
#include "fatal.hpp"
#include "foreach.hpp"
#include "gate.hpp"
#include "process.hpp"
#include "synchronized.hpp"

using boost::tuple;

using std::cerr;
using std::deque;
using std::endl;
using std::find;
using std::list;
using std::map;
using std::max;
using std::ostream;
using std::queue;
using std::set;
using std::stack;

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


struct node
{
  uint32_t ip;
  uint16_t port;
};


bool operator < (const node& left, const node& right)
{
  if (left.ip == right.ip)
    return left.port < right.port;
  else
    return left.ip < right.ip;
}


ostream& operator << (ostream& stream, const node& n)
{
  stream << n.ip << ":" << n.port;
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
  PID pid;
  int generation;
};


bool operator == (const timeout &left, const timeout &right)
{
  return left.tstamp == right.tstamp &&
    left.pid == right.pid &&
    left.generation == right.generation;
}


class ProcessReference
{
public:
  explicit ProcessReference(Process *_process) : process(_process)
  {
    if (process != NULL) {
      __sync_fetch_and_add(&(process->refs), 1);
      if (process->state == Process::EXITING) {
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
      assert(process->refs > 0);
      __sync_fetch_and_add(&(process->refs), 1);
    }
  }

  Process * operator -> ()
  {
    return process;
  }

  operator Process * ()
  {
    return process;
  }

  operator bool () const
  {
    return process != NULL;
  }

private:
  ProcessReference & operator = (const ProcessReference &that);

  Process *process;
};


class LinkManager
{
public:
  LinkManager();
  ~LinkManager();

  void link(Process *process, const PID &to);

  void send(struct msg *msg);

  struct msg * next(int s);
  struct msg * next_or_close(int s);
  struct msg * next_or_sleep(int s);

  void closed(int s);

  void exited(const node &n);
  void exited(Process *process);

private:
  /* Map from PID (local/remote) to process. */
  map<PID, set<Process *> > links;

  /* Map from socket to node (ip, port). */
  map<int, node> sockets;

  /* Maps from node (ip, port) to socket. */
  map<node, int> temps;
  map<node, int> persists;

  /* Map from socket to outgoing messages. */
  map<int, queue<struct msg *> > outgoing;

  /* Protects instance variables. */
  synchronizable(this);
};


class ProcessManager
{
public:
  ProcessManager();
  ~ProcessManager();

  ProcessReference use(const PID &pid);

  void record(struct msg *msg);
  void replay();

  void deliver(struct msg *msg, Process *sender = NULL);

  void spawn(Process *process);
  void link(Process *process, const PID &to);
  void receive(Process *process, double secs);
  void pause(Process *process, double secs);
  bool wait(Process *process, const PID &pid);
  bool external_wait(const PID &pid);
  bool await(Process *process, int fd, int op, double secs, bool ignore);

  void enqueue(Process *process);
  Process * dequeue();

  void timedout(const PID &pid, int generation);
  void awaited(const PID &pid, int generation);

  void run(Process *process);
  void cleanup(Process *process);

  static void invoke(const function<void (void)> &thunk);

private:
  timeout create_timeout(Process *process, double secs);
  void start_timeout(const timeout &timeout);
  void cancel_timeout(const timeout &timeout);

  /* Map of all local spawned and running processes. */
  map<uint32_t, Process *> processes;
  synchronizable(processes);

  /* Waiting processes (protected by synchronizable(processes)). */
  map<Process *, set<Process *> > waiters;

  /* Map of gates for waiting threads. */
  map<Process *, Gate *> gates;

  /* Queue of runnable processes (implemented as deque). */
  deque<Process *> runq;
  synchronizable(runq);
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

  ev_tstamp getCurrent(Process *process)
  {
    ev_tstamp tstamp;

    if (currents.count(process) != 0) {
      tstamp = currents[process];
    } else {
      tstamp = currents[process] = initial;
    }

    return tstamp;
  }

  void setCurrent(Process *process, ev_tstamp tstamp)
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

  void discard(Process *process)
  {
    assert(process != NULL);
    currents.erase(process);
  }

private:
  map<Process *, ev_tstamp> currents;
  ev_tstamp initial;
  ev_tstamp current;
  ev_tstamp elapsed;
};


/* Using manual clock if non-null. */
static InternalClock *clk = NULL;

/* Global 'pipe' id uniquely assigned to each process. */
static uint32_t global_pipe = 0;

/* Local server socket. */
static int s = -1;

/* Local IP address. */
static uint32_t ip = 0;

/* Local port. */
static uint16_t port = 0;

/* Active LinkManager (eventually will probably be thread-local). */
static LinkManager *link_manager = NULL;

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

/* Queue of new I/O watchers. */
static queue<ev_io *> *io_watchersq = new queue<ev_io *>();
static synchronizable(io_watchersq) = SYNCHRONIZED_INITIALIZER;

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
//static __thread Process *proc_process = NULL;
static Process *proc_process = NULL;

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

/* Record? */
static bool recording = false;

/* Record(s) for replay. */
static std::fstream record_msgs;
static std::fstream record_pipes;

/* Replay? */
static bool replaying = false;

/* Replay messages (id -> queue of messages). */
static map<uint32_t, queue<struct msg *> > *replay_msgs =
  new map<uint32_t, queue<struct msg *> >();

/* Replay pipes (parent id -> stack of remaining child ids). */
static map<uint32_t, deque<uint32_t> > *replay_pipes =
  new map<uint32_t, deque<uint32_t> >();

/**
 * Filter. Synchronized support for using the filterer needs to be
 * recursive incase a filterer wants to do anything fancy (which is
 * possible and likely given that filters will get used for testing).
*/
static Filter *filterer = NULL;
static synchronizable(filterer) = SYNCHRONIZED_INITIALIZER_RECURSIVE;



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


void handle_async(struct ev_loop *loop, ev_async *w, int revents)
{
  synchronized(io_watchersq) {
    /* Start all the new I/O watchers. */
    while (!io_watchersq->empty()) {
      ev_io *io_watcher = io_watchersq->front();
      io_watchersq->pop();
      ev_io_start(loop, io_watcher);
    }
  }

  synchronized(timeouts) {
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


void handle_timeout(struct ev_loop *loop, ev_timer *w, int revents)
{
  list<timeout> timedout;

  synchronized(timeouts) {
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
    assert(timeouts->empty() || (timeouts->begin()->first > current_tstamp));

    // Update the timer as necessary.
    // TODO(benh): Make this code look like the code in handle_async.
    if (!timeouts->empty() && clk == NULL) {
      timeouts_watcher.repeat = timeouts->begin()->first - current_tstamp;
      assert(timeouts_watcher.repeat > 0);
      ev_timer_again(loop, &timeouts_watcher);
    } else {
      timeouts_watcher.repeat = 0;
      ev_timer_again(loop, &timeouts_watcher);
    }

    update_timer = false;
  }

  foreach (const timeout &timeout, timedout)
    process_manager->timedout(timeout.pid, timeout.generation);
}


void handle_await(struct ev_loop *loop, ev_io *w, int revents)
{
  tuple<PID, int> *t = reinterpret_cast<tuple<PID, int> *>(w->data);
  process_manager->awaited(t->get<0>(), t->get<1>());
  ev_io_stop(loop, w);
  free(w);
  delete t;
}


/* Socket reading .... */
void read_data(struct ev_loop *loop, ev_io *w, int revents);
void read_msg(struct ev_loop *loop, ev_io *w, int revents);

struct read_ctx {
  int len;
  struct msg *msg;
};


void read_data(struct ev_loop *loop, ev_io *w, int revents)
{
  int c = w->fd;

  struct read_ctx *ctx = (struct read_ctx *) w->data;

  /* Read the data starting from the last read. */
  int len = recv(c,
		 (char *) ctx->msg + sizeof(struct msg) + ctx->len,
		 ctx->msg->len - ctx->len,
		 0);

  if (len > 0) {
    ctx->len += len;
  } else if (len < 0 && errno == EWOULDBLOCK) {
    return;
  } else if (len == 0 || (len < 0 &&
			  (errno == ECONNRESET ||
			   errno == EBADF ||
			   errno == EHOSTUNREACH))) {
    /* Socket has closed. */
    link_manager->closed(c);

    /* Stop receiving ... */
    ev_io_stop (loop, w);
    close(c);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  } else {
    fatalerror("unhandled socket error: please report (read_data)");
  }

  if (ctx->len == ctx->msg->len) {
    /* Deliver message. */
    process_manager->deliver(ctx->msg);

    /* Reinitialize read context. */
    ctx->len = 0;
    ctx->msg = (struct msg *) malloc(sizeof(struct msg));

    /* Continue receiving ... */
    ev_io_stop (loop, w);
    ev_io_init (w, read_msg, c, EV_READ);
    ev_io_start (loop, w);
  }
}


void read_msg(struct ev_loop *loop, ev_io *w, int revents)
{
  int c = w->fd;

  struct read_ctx *ctx = (struct read_ctx *) w->data;

  /* Read the message starting from the last read. */
  int len = recv(c,
		 (char *) ctx->msg + ctx->len,
		 sizeof (struct msg) - ctx->len,
		 0);

  if (len > 0) {
    ctx->len += len;
  } else if (len < 0 && errno == EWOULDBLOCK) {
    return;
  } else if (len == 0 || (len < 0 &&
			  (errno == ECONNRESET ||
			   errno == EBADF ||
			   errno == EHOSTUNREACH))) {
    /* Socket has closed. */
    link_manager->closed(c);

    /* Stop receiving ... */
    ev_io_stop (loop, w);
    close(c);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  } else {
    fatalerror("unhandled socket error: please report (read_msg)");
  }

  if (ctx->len == sizeof(struct msg)) {
    /* Check and see if we need to receive data. */
    if (ctx->msg->len > 0) {
      /* Allocate enough space for data. */
      ctx->msg = (struct msg *)
	realloc (ctx->msg, sizeof(struct msg) + ctx->msg->len);

      /* TODO(benh): Optimize ... try doing a read first! */
      ctx->len = 0;

      /* Start receiving data ... */
      ev_io_stop (loop, w);
      ev_io_init (w, read_data, c, EV_READ);
      ev_io_start (loop, w);
    } else {
      /* Deliver message. */
      process_manager->deliver(ctx->msg);

      /* Reinitialize read context. */
      ctx->len = 0;
      ctx->msg = (struct msg *) malloc(sizeof(struct msg));

      /* Continue receiving ... */
      ev_io_stop (loop, w);
      ev_io_init (w, read_msg, c, EV_READ);
      ev_io_start (loop, w);
    }
  }
}


/* Socket writing .... */
void write_data(struct ev_loop *loop, ev_io *w, int revents);
void write_msg(struct ev_loop *loop, ev_io *w, int revents);

struct write_ctx {
  int len;
  struct msg *msg;
  bool close;
};


void write_data(struct ev_loop *loop, ev_io *w, int revents)
{
  int c = w->fd;

  struct write_ctx *ctx = (struct write_ctx *) w->data;

  int len = send(c,
		 (char *) ctx->msg + sizeof(struct msg) + ctx->len,
		 ctx->msg->len - ctx->len,
		 MSG_NOSIGNAL);

  if (len > 0) {
    ctx->len += len;
  } else if (len < 0 && errno == EWOULDBLOCK) {
    return;
  } else if (len == 0 || (len < 0 &&
			  (errno == ECONNRESET ||
			   errno == EBADF ||
			   errno == EHOSTUNREACH ||
			   errno == EPIPE))) {
    /* Socket has closed. */
    link_manager->closed(c);

    /* Stop receiving ... */
    ev_io_stop (loop, w);
    close(c);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  } else {
    fatalerror("unhandled socket error: please report (write_data)");
  }

  if (ctx->len == ctx->msg->len) {
    ev_io_stop (loop, w);
    free(ctx->msg);

    if (ctx->close)
      ctx->msg = link_manager->next_or_close(c);
    else
      ctx->msg = link_manager->next_or_sleep(c);

    if (ctx->msg != NULL) {
      ctx->len = 0;
      ev_io_init(w, write_msg, c, EV_WRITE);
      ev_io_start(loop, w);
    } else {
      free(ctx);
      free(w);
    }
  }
}


void write_msg(struct ev_loop *loop, ev_io *w, int revents)
{
  int c = w->fd;

  struct write_ctx *ctx = (struct write_ctx *) w->data;

  int len = send(c,
		 (char *) ctx->msg + ctx->len,
		 sizeof (struct msg) - ctx->len,
		 MSG_NOSIGNAL);

  if (len > 0) {
    ctx->len += len;
  } else if (len < 0 && errno == EWOULDBLOCK) {
    return;
  } else if (len == 0 || (len < 0 &&
			  (errno == ECONNRESET ||
			   errno == EBADF ||
			   errno == EHOSTUNREACH ||
			   errno == EPIPE))) {
    /* Socket has closed. */
    link_manager->closed(c);

    /* Stop receiving ... */
    ev_io_stop (loop, w);
    close(c);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  } else {
    fatalerror("unhandled socket error: please report (write_msg)");
  }

  if (ctx->len == sizeof(struct msg)) {
    /* Check and see if we need to write data. */
    if (ctx->msg->len > 0) {
      
      /* TODO(benh): Optimize ... try doing a write first! */
      ctx->len = 0;

      /* Start writing data ... */
      ev_io_stop(loop, w);
      ev_io_init(w, write_data, c, EV_WRITE);
      ev_io_start(loop, w);
    } else {
      ev_io_stop(loop, w);
      free(ctx->msg);

      if (ctx->close)
	ctx->msg = link_manager->next_or_close(c);
      else
	ctx->msg = link_manager->next_or_sleep(c);

      if (ctx->msg != NULL) {
	ctx->len = 0;
	ev_io_init(w, write_msg, c, EV_WRITE);
	ev_io_start(loop, w);
      } else {
	free(ctx);
	free(w);
      }
    }
  }
}


void write_connect(struct ev_loop *loop, ev_io *w, int revents)
{
  int s = w->fd;

  struct write_ctx *ctx = (struct write_ctx *) w->data;

  ev_io_stop(loop, w);

  /* Check that the connection was successful. */
  int opt;
  socklen_t optlen = sizeof(opt);

  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0) {
    link_manager->closed(s);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  }

  if (opt != 0) {
    link_manager->closed(s);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  }

  /* TODO(benh): Optimize ... try doing a write first. */

  ev_io_init(w, write_msg, s, EV_WRITE);
  ev_io_start(loop, w);
}



void link_connect(struct ev_loop *loop, ev_io *w, int revents)
{
  int s = w->fd;

  ev_io_stop(loop, w);

  /* Check that the connection was successful. */
  int opt;
  socklen_t optlen = sizeof(opt);

  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0) {
    link_manager->closed(s);
    free(w);
    return;
  }

  if (opt != 0) {
    link_manager->closed(s);
    free(w);
    return;
  }

  /* Reuse/Initialize the watcher. */
  w->data = malloc(sizeof(struct read_ctx));

  /* Initialize read context. */
  struct read_ctx *ctx = (struct read_ctx *) w->data;

  ctx->len = 0;
  ctx->msg = (struct msg *) malloc(sizeof(struct msg));

  /* Initialize watcher for reading. */
  ev_io_init(w, read_msg, s, EV_READ);

  ev_io_start(loop, w);
}


void do_accept(struct ev_loop *loop, ev_io *w, int revents)
{
  int s = w->fd;

  struct sockaddr_in addr;

  socklen_t addrlen = sizeof(addr);

  /* Do accept. */
  int c = accept(s, (struct sockaddr *) &addr, &addrlen);

  if (c < 0) {
    return;
  }

  /* Make socket non-blocking. */
  if (set_nbio(c) < 0) {
    close(c);
    return;
  }

  /* Turn off Nagle (on TCP_NODELAY) so pipelined requests don't wait. */
  int on = 1;
  if (setsockopt(c, SOL_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    close(c);
    return;
  }

  /* Allocate the watcher. */
  ev_io *io_watcher = (ev_io *) malloc (sizeof (ev_io));

  io_watcher->data = malloc(sizeof(struct read_ctx));

  /* Initialize the read context */
  struct read_ctx *ctx = (struct read_ctx *) io_watcher->data;

  ctx->len = 0;
  ctx->msg = (struct msg *) malloc(sizeof(struct msg));

  /* Initialize watcher for reading. */
  ev_io_init(io_watcher, read_msg, c, EV_READ);

  ev_io_start(loop, io_watcher);
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
  assert (sizeof(unsigned long) == sizeof(Process *));
  void *stack = (void *)
    (((unsigned long) stack1 << 32) + (unsigned int) stack0);
  Process *process = (Process *)
    (((unsigned long) process1 << 32) + (unsigned int) process0);
#else
  assert (sizeof(unsigned int) == sizeof(Process *));
  void *stack = (void *) (unsigned int) stack0;
  Process *process = (Process *) (unsigned int) process0;
#endif /* __x86_64__ */

  /* Run the process. */
  process_manager->run(process);

  /* Prepare to recycle this stack (global variable hack!). */
  assert(recyclable == NULL);
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
    synchronized(stacks) {
      stacks->push(recyclable);
    }
    recyclable = NULL;
  }

  do {
    if (replaying)
      process_manager->replay();

    Process *process = process_manager->dequeue();

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
        // it is awaiting has become ready, or (3) if it has a
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

        synchronized(timeouts) {
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
              // descriptor that a process is awaiting. We may want to
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
      assert(process->state == Process::INIT ||
	     process->state == Process::READY ||
	     process->state == Process::INTERRUPTED ||
	     process->state == Process::TIMEDOUT);

      /* Continue process. */
      assert(proc_process == NULL);
      proc_process = process;
      swapcontext(&proc_uctx_running, &process->uctx);
      while (legacy) {
	(*legacy_thunk)();
	swapcontext(&proc_uctx_running, &process->uctx);
      }
      assert(proc_process != NULL);
      proc_process = NULL;
    }
    process->unlock();
  } while (true);
}


/*
 * We might need/want to catch terminating signals to close our log
 * ... or the underlying filesystem and operating system might be
 * robust enough to flush our last writes and close the file cleanly,
 * or we might need to force flushes at appropriate times. However,
 * for now, adding signal handlers freely is not allowed because they
 * will clash with Java and Python virtual machines and causes hard to
 * debug crashes/segfaults. This can be revisited when recording gets
 * turned on by default.
 */


// void sigbad(int signal, struct sigcontext *ctx)
// {
//   if (recording) {
//     assert(!replaying);
//     record_msgs.close();
//     record_pipes.close();
//   }

//   /* Pass on the signal (so that a core file is produced).  */
//   struct sigaction sa;
//   sa.sa_handler = SIG_DFL;
//   sigemptyset(&sa.sa_mask);
//   sa.sa_flags = 0;
//   sigaction(signal, &sa, NULL);
//   raise(signal);
// }


void initialize()
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
#endif /* __sun__ */

  /* Create a new ProcessManager and LinkManager. */
  process_manager = new ProcessManager();
  link_manager = new LinkManager();

  /* Setup processing thread. */
  if (pthread_create (&proc_thread, NULL, schedule, NULL) != 0)
    fatalerror("failed to initialize (pthread_create)");

  ip = 0;
  port = 0;

  char *value;

  /* Check environment for ip. */
  value = getenv("LIBPROCESS_IP");
  if (value != NULL) {
    int result = inet_pton(AF_INET, value, &ip);
    if (result == 0) {
      fatal("LIBPROCESS_IP=%s was unparseable", value);
    } else if (result < 0) {
      fatalerror("failed to initialize (inet_pton)");
    }
  }

  /* Check environment for port. */
  value = getenv("LIBPROCESS_PORT");
  if (value != NULL) {
    int result = atoi(value);
    if (result < 0 || result > USHRT_MAX) {
      fatal("LIBPROCESS_PORT=%s is not a valid port", value);
    }
    port = result;
  }

  /* Check environment for replay. */
  value = getenv("LIBPROCESS_REPLAY");
  replaying = value != NULL;

  /* Setup for recording or replaying. */
  if (recording && !replaying) {
    /* Setup record. */
    time_t t;
    time(&t);
    std::string record(".record-");
    std::string date(ctime(&t));

    replace(date.begin(), date.end(), ' ', '_');
    replace(date.begin(), date.end(), '\n', '\0');

    /* TODO(benh): Create file if it doesn't exist. */
    record_msgs.open((record + "msgs-" + date).c_str(),
		     std::ios::out | std::ios::binary | std::ios::app);
    record_pipes.open((record + "pipes-" + date).c_str(),
		      std::ios::out | std::ios::app);
    if (!record_msgs.is_open() || !record_pipes.is_open())
      fatal("could not open record(s) for recording");
  } else if (replaying) {
    assert(!recording);
    /* Open record(s) for replay. */
    record_msgs.open((std::string(".record-msgs-") += value).c_str(),
		     std::ios::in | std::ios::binary);
    record_pipes.open((std::string(".record-pipes-") += value).c_str(),
		      std::ios::in);
    if (!record_msgs.is_open() || !record_pipes.is_open())
      fatal("could not open record(s) with prefix %s for replay", value);

    /* Read in all pipes from record. */
    while (!record_pipes.eof()) {
      uint32_t parent, child;
      record_pipes >> parent >> child;
      if (record_pipes.fail())
	fatal("could not read from record");
      (*replay_pipes)[parent].push_back(child);
    }

    record_pipes.close();
  }

  /* TODO(benh): Check during replay that the same ip and port is used. */

  // Lookup hostname if missing ip (avoids getting 127.0.0.1). Note
  // that we need only one ip address, so that other processes can
  // send and receive and don't get confused as to whom they are
  // sending to.
  if (ip == 0) {
    char hostname[512];

    if (gethostname(hostname, sizeof(hostname)) < 0)
      fatalerror("failed to initialize (gethostname)");

    /* Lookup IP address of local hostname. */
    struct hostent *he;

    if ((he = gethostbyname2(hostname, AF_INET)) == NULL)
      fatalerror("failed to initialize (gethostbyname2)");

    ip = *((uint32_t *) he->h_addr_list[0]);
  }

  /* Create a "server" socket for communicating with other nodes. */
  if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)
    fatalerror("failed to initialize (socket)");

  /* Make socket non-blocking. */
  if (set_nbio(s) < 0)
    fatalerror("failed to initialize (set_nbio)");

  /* Allow socket reuse. */
  int on = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
    fatalerror("failed to initialize (setsockopt(SO_REUSEADDR) failed)");

  /* Set up socket. */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = PF_INET;
  addr.sin_addr.s_addr = ip;
  addr.sin_port = htons(port);

  if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    fatalerror("failed to initialize (bind)");

  /* Lookup and store assigned ip and assigned port. */
  socklen_t addrlen = sizeof(addr);
  if (getsockname(s, (struct sockaddr *) &addr, &addrlen) < 0)
    fatalerror("failed to initialize (getsockname)");

  ip = addr.sin_addr.s_addr;
  port = ntohs(addr.sin_port);

  if (listen(s, 500000) < 0)
    fatalerror("failed to initialize (listen)");

  /* Setup event loop. */
#ifdef __sun__
  loop = ev_default_loop(EVBACKEND_POLL | EVBACKEND_SELECT);
#else
  loop = ev_default_loop(EVFLAG_AUTO);
#endif /* __sun__ */

  ev_async_init(&async_watcher, handle_async);
  ev_async_start(loop, &async_watcher);

  ev_timer_init(&timeouts_watcher, handle_timeout, 0., 2100000.0);
  ev_timer_again(loop, &timeouts_watcher);

  ev_io_init(&server_watcher, do_accept, s, EV_READ);
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

  if (pthread_create(&io_thread, NULL, serve, loop) != 0)
    fatalerror("failed to initialize node (pthread_create)");

  initializing = false;
}


LinkManager::LinkManager()
{
  synchronizer(this) = SYNCHRONIZED_INITIALIZER_RECURSIVE;
}


LinkManager::~LinkManager() {}


void LinkManager::link(Process *process, const PID &to)
{
  // TODO(benh): The semantics we want to support for link are such
  // that if there is nobody to link to (local or remote) then a
  // PROCESS_EXIT message gets generated. This functionality has only
  // been implemented when the link is local, not remote. Of course,
  // if there is nobody listening on the remote side, then this should
  // work remotely ... but if there is someone listening remotely just
  // not at that pipe value, then it will silently continue executing.

  assert(process != NULL);

  const node n = { to.ip, to.port };

  synchronized(this) {
    // Check if node is remote and there isn't a persistant link.
    if ((n.ip != ip || n.port != port) &&
        persists.find(n) == persists.end()) {
      int s;

      /* Create socket for communicating with remote process. */
      if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)
        fatalerror("failed to link (socket)");
    
      /* Use non-blocking sockets. */
      if (set_nbio(s) < 0)
        fatalerror("failed to link (set_nbio)");

      /* Record socket. */
      sockets[s] = n;

      /* Record node. */
      persists[n] = s;

      /* Allocate the watcher. */
      ev_io *io_watcher = (ev_io *) malloc(sizeof(ev_io));

      struct sockaddr_in addr;
      
      memset(&addr, 0, sizeof(addr));
      
      addr.sin_family = PF_INET;
      addr.sin_port = htons(to.port);
      addr.sin_addr.s_addr = to.ip;

      if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS)
          fatalerror("failed to link (connect)");

        /* Initialize watcher for connecting. */
        ev_io_init(io_watcher, link_connect, s, EV_WRITE);
      } else {
        /* Initialize watcher for reading. */
        io_watcher->data = malloc(sizeof(struct read_ctx));

        /* Initialize read context. */
        struct read_ctx *ctx = (struct read_ctx *) io_watcher->data;

        ctx->len = 0;
        ctx->msg = (struct msg *) malloc(sizeof(struct msg));

        ev_io_init(io_watcher, read_msg, s, EV_READ);
      }

      /* Enqueue the watcher. */
      synchronized(io_watchersq) {
        io_watchersq->push(io_watcher);
      }

      /* Interrupt the loop. */
      ev_async_send(loop, &async_watcher);
    }

    links[to].insert(process);
  }
}


void LinkManager::send(struct msg *msg)
{
  assert(msg != NULL);

  node n = { msg->to.ip, msg->to.port };

  synchronized(this) {
    // Check if there is already a link.
    map<node, int>::iterator it;
    if ((it = persists.find(n)) != persists.end() ||
        (it = temps.find(n)) != temps.end()) {
      int s = it->second;
      if (outgoing.count(s) == 0) {
        assert(persists.count(n) != 0);
        assert(temps.count(n) == 0 || temps[n] != s);

        /* Initialize the outgoing queue. */
        outgoing[s];

        /* Allocate/Initialize the watcher. */
        ev_io *io_watcher = (ev_io *) malloc (sizeof (ev_io));

        io_watcher->data = malloc(sizeof(struct write_ctx));

        /* Initialize the write context. */
        struct write_ctx *ctx = (struct write_ctx *) io_watcher->data;

        ctx->len = 0;
        ctx->msg = msg;
        ctx->close = false;

        ev_io_init(io_watcher, write_msg, s, EV_WRITE);

        /* Enqueue the watcher. */
        synchronized(io_watchersq) {
          io_watchersq->push(io_watcher);
        }
    
        /* Interrupt the loop. */
        ev_async_send(loop, &async_watcher);
      } else {
        outgoing[s].push(msg);
      }
    } else {
      int s;

      /* Create socket for communicating with remote process. */
      if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)
        fatalerror("failed to send (socket)");
    
      /* Use non-blocking sockets. */
      if (set_nbio(s) < 0)
        fatalerror("failed to send (set_nbio)");

      /* Record socket. */
      sockets[s] = n;

      /* Record node. */
      temps[n] = s;

      /* Initialize the outgoing queue. */
      outgoing[s];

      /* Allocate/Initialize the watcher. */
      ev_io *io_watcher = (ev_io *) malloc (sizeof (ev_io));

      io_watcher->data = malloc(sizeof(struct write_ctx));

      /* Initialize the write context. */
      struct write_ctx *ctx = (struct write_ctx *) io_watcher->data;

      ctx->len = 0;
      ctx->msg = msg;
      ctx->close = true;

      struct sockaddr_in addr;

      memset(&addr, 0, sizeof(addr));
      
      addr.sin_family = PF_INET;
      addr.sin_port = htons(msg->to.port);
      addr.sin_addr.s_addr = msg->to.ip;
    
      if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS)
          fatalerror("failed to send (connect)");

        /* Initialize watcher for connecting. */
        ev_io_init(io_watcher, write_connect, s, EV_WRITE);
      } else {
        /* Initialize watcher for writing. */
        ev_io_init(io_watcher, write_msg, s, EV_WRITE);
      }

      /* Enqueue the watcher. */
      synchronized(io_watchersq) {
        io_watchersq->push(io_watcher);
      }

      /* Interrupt the loop. */
      ev_async_send(loop, &async_watcher);
    }
  }
}


struct msg * LinkManager::next(int s)
{
  struct msg *msg = NULL;

  synchronized(this) {
    assert(outgoing.find(s) != outgoing.end());
    if (!outgoing[s].empty()) {
      msg = outgoing[s].front();
      outgoing[s].pop();
    }
  }

  return msg;
}


struct msg * LinkManager::next_or_close(int s)
{
  struct msg *msg;

  synchronized(this) {
    if ((msg = next(s)) == NULL) {
      assert(outgoing[s].empty());
      outgoing.erase(s);
      assert(temps.count(sockets[s]) > 0);
      temps.erase(sockets[s]);
      sockets.erase(s);
      close(s);
    }
  }

  return msg;
}


struct msg * LinkManager::next_or_sleep(int s)
{
  struct msg *msg;

  synchronized(this) {
    if ((msg = next(s)) == NULL) {
      assert(outgoing[s].empty());
      outgoing.erase(s);
      assert(persists.find(sockets[s]) != persists.end());
    }
  }

  return msg;
}


void LinkManager::closed(int s)
{
  synchronized(this) {
    if (sockets.count(s) > 0) {
      const node &n = sockets[s];

      // Don't bother invoking exited unless socket was from persists.
      if (persists.count(n) > 0 && persists[n] == s) {
	persists.erase(n);
	exited(n);
      } else {
	assert(temps.count(n) > 0 && temps[n] == s);
	temps.erase(n);
      }

      sockets.erase(s);
      outgoing.erase(s);
      close(s);
    }
  }
}


void LinkManager::exited(const node &n)
{
  // TODO(benh): It would be cleaner if this routine could call back
  // into ProcessManager ... then we wouldn't have to convince
  // ourselves that the accesses to each Process object will always be
  // valid.
  synchronized(this) {
    list<PID> removed;
    /* Look up all linked processes. */
    foreachpair (const PID &pid, set<Process *> &processes, links) {
      if (pid.ip == n.ip && pid.port == n.port) {
        /* N.B. If we call exited(pid) we might invalidate iteration. */
        /* Deliver PROCESS_EXIT messages (if we aren't replaying). */
        if (!replaying) {
          foreach (Process *process, processes) {
            struct msg *msg = (struct msg *) malloc(sizeof(struct msg));
            msg->from.pipe = pid.pipe;
            msg->from.ip = pid.ip;
            msg->from.port = pid.port;
            msg->to.pipe = process->pid.pipe;
            msg->to.ip = process->pid.ip;
            msg->to.port = process->pid.port;
            msg->id = PROCESS_EXIT;
            msg->len = 0;
            process->enqueue(msg);
          }
        }
        removed.push_back(pid);
      }
    }
    foreach (const PID &pid, removed)
      links.erase(pid);
  }
}


void LinkManager::exited(Process *process)
{
  synchronized(this) {
    /* Remove any links this process might have had. */
    foreachpair (_, set<Process *> &processes, links)
      processes.erase(process);

    const PID &pid = process->self();

    /* Look up all linked processes. */
    map<PID, set<Process *> >::iterator it = links.find(pid);

    if (it != links.end()) {
      set<Process *> &processes = it->second;
      /* Deliver PROCESS_EXIT messages (if we aren't replaying). */
      if (!replaying) {
        foreach (Process *p, processes) {
          assert(process != p);
          struct msg *msg = (struct msg *) malloc(sizeof(struct msg));
          msg->from.pipe = pid.pipe;
          msg->from.ip = pid.ip;
          msg->from.port = pid.port;
          msg->to.pipe = p->pid.pipe;
          msg->to.ip = p->pid.ip;
          msg->to.port = p->pid.port;
          msg->id = PROCESS_EXIT;
          msg->len = 0;
          // TODO(benh): Preserve happens-before when using clock.
          p->enqueue(msg);
        }
      }
      links.erase(pid);
    }
  }
}


ProcessManager::ProcessManager()
{
  synchronizer(processes) = SYNCHRONIZED_INITIALIZER;
  synchronizer(runq) = SYNCHRONIZED_INITIALIZER;
}


ProcessManager::~ProcessManager() {}


ProcessReference ProcessManager::use(const PID &pid)
{
  if (pid.ip == ip && pid.port == port) {
    synchronized(processes) {
      if (processes.count(pid.pipe) > 0) {
        // Note that the ProcessReference constructor MUST get called
        // while holding the lock on processes.
        return ProcessReference(processes[pid.pipe]);
      }
    }
  }

  return ProcessReference(NULL);
}


void ProcessManager::record(struct msg *msg)
{
  assert(recording && !replaying);
  synchronized(processes) {
    record_msgs.write((char *) msg, sizeof(struct msg) + msg->len);
    if (record_msgs.fail())
      fatalerror("failed to write to messages record");
  }
}


void ProcessManager::replay()
{
  assert(!recording && replaying);
  synchronized(processes) {
    if (!record_msgs.eof()) {
      struct msg *msg = (struct msg *) malloc(sizeof(struct msg));

      /* Read a message worth of data. */
      record_msgs.read((char *) msg, sizeof(struct msg));

      if (record_msgs.eof()) {
        free(msg);
        return;
      }

      if (record_msgs.fail())
        fatalerror("failed to read from messages record");

      /* Read the body of the message if necessary. */
      if (msg->len != 0) {
        struct msg *temp = msg;
        msg = (struct msg *) malloc(sizeof(struct msg) + msg->len);
        memcpy(msg, temp, sizeof(struct msg));
        free(temp);
        record_msgs.read((char *) msg + sizeof(struct msg), msg->len);
        if (record_msgs.fail())
          fatalerror("failed to read from messages record");
      }

      /* Add message to be delivered later. */
      (*replay_msgs)[msg->to.pipe].push(msg);
    }

    /* Deliver any messages to available processes. */
    foreachpair (uint32_t pipe, Process *process, processes) {
      queue<struct msg *> &msgs = (*replay_msgs)[pipe];
      while (!msgs.empty()) {
        struct msg *msg = msgs.front();
        msgs.pop();
        process->enqueue(msg);
      }
    }
  }
}


void ProcessManager::deliver(struct msg *msg, Process *sender)
{
  assert(msg != NULL);
  assert(!replaying);

  if (ProcessReference receiver = use(msg->to)) {
    // If we have a local sender AND we are using a manual clock
    // then update the current time of the receiver to preserve
    // the happens-before relationship between the sender and
    // receiver. Note that the assumption is that the sender
    // remains valid for at least the duration of this routine (so
    // that we can look up it's current time).
    if (sender != NULL) {
      synchronized(timeouts) {
        if (clk != NULL) {
          clk->setCurrent(receiver, max(clk->getCurrent(receiver),
                                        clk->getCurrent(sender)));
        }
      }
    }

    receiver->enqueue(msg);
  } else {
    free(msg);
  }
}


void ProcessManager::spawn(Process *process)
{
  assert(process != NULL);

  process->state = Process::INIT;

  /* Record process. */
  synchronized(processes) {
    processes[process->pid.pipe] = process;
  }

  void *stack = NULL;

  // Reuse a stack if any are available.
  synchronized(stacks) {
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
  assert(sizeof(unsigned long) == sizeof(Process *));
  int stack0 = (unsigned int) (unsigned long) stack;
  int stack1 = (unsigned long) stack >> 32;
  int process0 = (unsigned int) (unsigned long) process;
  int process1 = (unsigned long) process >> 32;
#else
  assert(sizeof(unsigned int) == sizeof(Process *));
  int stack0 = (unsigned int) stack;
  int stack1 = 0;
  int process0 = (unsigned int) process;
  int process1 = 0;
#endif /* __x86_64__ */

  makecontext(&process->uctx, (void (*)()) trampoline,
              4, stack0, stack1, process0, process1);

  /* Add process to the run queue. */
  enqueue(process);
}



void ProcessManager::link(Process *process, const PID &to)
{
  // Check if the pid is local.
  if (!(to.ip == ip && to.port == port)) {
    link_manager->link(process, to);
  } else {
    // Since the pid is local we want to get a reference to it's
    // underlying process so that while we are invoking the link
    // manager we don't miss sending a possible PROCESS_EXIT.
    if (ProcessReference _ = use(to)) {
      link_manager->link(process, to);
    } else {
      // Since the pid isn't valid it's process must have already died
      // (or hasn't been spawned yet) so send a process exit message.
      struct msg *msg = (struct msg *) malloc(sizeof(struct msg));
      msg->from.pipe = to.pipe;
      msg->from.ip = to.ip;
      msg->from.port = to.port;
      msg->to.pipe = process->pid.pipe;
      msg->to.ip = process->pid.ip;
      msg->to.port = process->pid.port;
      msg->id = PROCESS_EXIT;
      msg->len = 0;
      process->enqueue(msg);
    }
  }
}


void ProcessManager::receive(Process *process, double secs)
{
  assert(process != NULL);
  process->lock();
  {
    /* Ensure nothing enqueued since check in Process::receive. */
    if (process->msgs.empty()) {
      if (secs > 0) {
        /* Create timeout. */
        const timeout &timeout = create_timeout(process, secs);

        /* Start the timeout. */
        start_timeout(timeout);

        /* Context switch. */
        process->state = Process::RECEIVING;
        swapcontext(&process->uctx, &proc_uctx_running);

        assert(process->state == Process::READY ||
               process->state == Process::TIMEDOUT);

        /* Attempt to cancel the timer if necessary. */
        if (process->state != Process::TIMEDOUT)
          cancel_timeout(timeout);

        /* N.B. No cancel means possible unnecessary timeouts. */

        process->state = Process::RUNNING;
      
        /* Update the generation (handles racing timeouts). */
        process->generation++;
      } else {
        /* Context switch. */
        process->state = Process::RECEIVING;
        swapcontext(&process->uctx, &proc_uctx_running);
        assert(process->state == Process::READY);
        process->state = Process::RUNNING;
      }
    }
  }
  process->unlock();
}


void ProcessManager::pause(Process *process, double secs)
{
  assert(process != NULL);

  process->lock();
  {
    if (secs > 0) {
      /* Create/Start the timeout. */
      start_timeout(create_timeout(process, secs));

      /* Context switch. */
      process->state = Process::PAUSED;
      swapcontext(&process->uctx, &proc_uctx_running);
      assert(process->state == Process::TIMEDOUT);
      process->state = Process::RUNNING;
    } else {
      /* Modified context switch (basically a yield). */
      process->state = Process::READY;
      enqueue(process);
      swapcontext(&process->uctx, &proc_uctx_running);
      assert(process->state == Process::READY);
      process->state = Process::RUNNING;
    }
  }
  process->unlock();
}


bool ProcessManager::wait(Process *process, const PID &pid)
{
  bool waited = false;

  /* Now we can add the process to the waiters. */
  synchronized(processes) {
    if (processes.count(pid.pipe) > 0) {
      assert(processes[pid.pipe]->state != Process::EXITED);
      waiters[processes[pid.pipe]].insert(process);
      waited = true;
    }
  }

  /* If we waited then we should context switch. */
  if (waited) {
    process->lock();
    {
      if (process->state == Process::RUNNING) {
        /* Context switch. */
        process->state = Process::WAITING;
        swapcontext(&process->uctx, &proc_uctx_running);
        assert(process->state == Process::READY);
        process->state = Process::RUNNING;
      } else {
        /* Process is cleaned up and we have been removed from waiters. */
        assert(process->state == Process::INTERRUPTED);
        process->state = Process::RUNNING;
      }
    }
    process->unlock();
  }

  return waited;
}


bool ProcessManager::external_wait(const PID &pid)
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
  synchronized(processes) {
    if (processes.count(pid.pipe) > 0) {
      Process *process = processes[pid.pipe];
      assert(process->state != Process::EXITED);

      /* Check and see if a gate already exists. */
      if (gates.find(process) == gates.end())
        gates[process] = new Gate();
      gate = gates[process];
      old = gate->approach();
    }
  }

  /* Now arrive at the gate and wait until it opens. */
  if (gate != NULL) {
    gate->arrive(old);
    if (gate->empty())
      delete gate;
    return true;
  }

  return false;
}


bool ProcessManager::await(Process *process, int fd, int op, double secs, bool ignore)
{
  assert(process != NULL);

  bool interrupted = false;

  process->lock();
  {
    /* Consider a non-empty message queue as an immediate interrupt. */
    if (!ignore && !process->msgs.empty()) {
      process->unlock();
      return false;
    }

    // Treat an await with a bad fd as an interruptible pause!
    if (fd >= 0) {
      /* Allocate/Initialize the watcher. */
      ev_io *io_watcher = (ev_io *) malloc(sizeof(ev_io));

      if ((op & Process::RDWR) == Process::RDWR)
        ev_io_init(io_watcher, handle_await, fd, EV_READ | EV_WRITE);
      else if ((op & Process::RDONLY) == Process::RDONLY)
        ev_io_init(io_watcher, handle_await, fd, EV_READ);
      else if ((op & Process::WRONLY) == Process::WRONLY)
        ev_io_init(io_watcher, handle_await, fd, EV_WRITE);

      // Tuple describing state (on heap in case we can't "cancel" it,
      // the watcher will always fire, even if we get interrupted and
      // return early, so this tuple will get cleaned up when the
      // watcher runs).
      io_watcher->data = new tuple<PID, int>(process->pid, process->generation);

      /* Enqueue the watcher. */
      synchronized(io_watchersq) {
        io_watchersq->push(io_watcher);
      }
    
      /* Interrupt the loop. */
      ev_async_send(loop, &async_watcher);
    }

    assert(secs >= 0);

    timeout timeout;

    if (secs != 0) {
      timeout = create_timeout(process, secs);
      start_timeout(timeout);
    }

    /* Context switch. */
    process->state = Process::AWAITING;
    swapcontext(&process->uctx, &proc_uctx_running);
    assert(process->state == Process::READY ||
           process->state == Process::TIMEDOUT ||
           process->state == Process::INTERRUPTED);

    /* Attempt to cancel the timer if necessary. */
    if (secs != 0)
      if (process->state != Process::TIMEDOUT)
        cancel_timeout(timeout);

    if (process->state == Process::INTERRUPTED)
      interrupted = true;

    process->state = Process::RUNNING;
      
    /* Update the generation (handles racing awaited). */
    process->generation++;
  }
  process->unlock();

  return !interrupted;
}


void ProcessManager::enqueue(Process *process)
{
  assert(process != NULL);
  synchronized(runq) {
    assert(find(runq.begin(), runq.end(), process) == runq.end());
    runq.push_back(process);
  }
    
  /* Wake up the processing thread if necessary. */
  gate->open();
}


Process * ProcessManager::dequeue()
{
  Process *process = NULL;

  synchronized(runq) {
    if (!runq.empty()) {
      process = runq.front();
      runq.pop_front();
    }
  }

  return process;
}


void ProcessManager::timedout(const PID &pid, int generation)
{
  if (ProcessReference process = use(pid)) {
    process->lock();
    {
      // We know we timed out if the state != READY after a timeout
      // but the generation is still the same.
      if (process->state != Process::READY &&
          process->generation == generation) {

        // The process could be in any of the following states,
        // including RUNNING if a pause, receive, or await was
        // initiated by an "outside" thread (e.g., in the constructor
        // of the process).
        assert(process->state == Process::RUNNING ||
               process->state == Process::RECEIVING ||
               process->state == Process::AWAITING ||
               process->state == Process::INTERRUPTED ||
               process->state == Process::PAUSED);

        if (process->state != Process::RUNNING ||
            process->state != Process::INTERRUPTED ||
            process->state != Process::EXITING)
          process_manager->enqueue(process);

        // We always have a timeout override the state (unless we are
        // exiting). This includes overriding INTERRUPTED. This means
        // that a process that was awaiting when selected from the
        // runq will fall out because of a timeout even though it also
        // received a message.
        if (process->state != Process::EXITING)
          process->state = Process::TIMEDOUT;
      }
    }
    process->unlock();
  }
}


void ProcessManager::awaited(const PID &pid, int generation)
{
  if (ProcessReference process = use(pid)) {
    process->lock();
    {
      if (process->state == Process::AWAITING &&
          process->generation == generation) {
        process->state = Process::READY;
        enqueue(process);
      }
    }
    process->unlock();
  }
}


void ProcessManager::run(Process *process)
{
  // Each process gets locked before 'schedule' runs it to enforce
  // atomicity for the blocking routines (receive, await, pause,
  // etc). So, we only need to unlock the process here.
  {
    process->state = Process::RUNNING;
  }
  process->unlock();

  try {
    (*process)();
  } catch (const std::exception &e) {
    cerr << "libprocess: " << process->pid
         << " exited due to "
         << e.what() << endl;
  } catch (...) {
    cerr << "libprocess: " << process->pid
         << " exited due to unknown exception" << endl;
  }

  cleanup(process);
}


void ProcessManager::cleanup(Process *process)
{
  /* Processes that were waiting on exiting process. */
  list<Process *> resumable;

  /* Possible gate non-libprocess threads are waiting at. */
  Gate *gate = NULL;

  /* Stop new process references from being created. */
  process->state = Process::EXITING;

  /* Remove process. */
  synchronized(processes) {
    /* Remove from internal clock (if necessary). */
    synchronized(timeouts) {
      if (clk != NULL)
        clk->discard(process);
    }

    /* Wait for all process references to get cleaned up. */
    while (process->refs > 0) {
      asm ("pause");
      __sync_synchronize();
    }

    process->lock();
    {
      /* Free any pending messages. */
      while (!process->msgs.empty()) {
        struct msg *msg = process->msgs.front();
        process->msgs.pop_front();
        free(msg);
      }

      /* Free current message. */
      if (process->current) free(process->current);

      processes.erase(process->pid.pipe);

      /* Confirm that the process is not in any waiting queue. */
      foreachpair (_, set<Process *> &waiting, waiters)
        assert(waiting.find(process) == waiting.end());

      /* Grab all the waiting processes that are now resumable. */
      foreach (Process *waiter, waiters[process])
        resumable.push_back(waiter);

      waiters.erase(process);

      /* Lookup gate to wake up waiting non-libprocess threads. */
      map<Process *, Gate *>::iterator it = gates.find(process);
      if (it != gates.end()) {
        gate = it->second;
        /* N.B. The last thread that leaves the gate also free's it. */
        gates.erase(it);
      }
        
      assert(process->refs == 0);
      process->state = Process::EXITED;
    }
    process->unlock();
  }

  /* Inform link manager. */
  link_manager->exited(process);

  /* Confirm process not in runq. */
  synchronized(runq) {
    assert(find(runq.begin(), runq.end(), process) == runq.end());
  }

  /*
   * N.B. After opening the gate we can no longer dereference
   * 'process' since it might already be cleaned up by user code (a
   * waiter might have cleaned up the stack where the process was
   * allocated).
   */
  if (gate != NULL)
    gate->open();

  /* And resume all processes waiting too. */
  foreach (Process *p, resumable) {
    p->lock();
    {
      // Process 'p' might be RUNNING because it is racing to become
      // WAITING while we are actually trying to get it to become
      // running again.
      // TODO(benh): Once we actually run multiple processes at a
      // time (using multiple threads) this logic will need to get
      // made thread safe (in particular, a process may be
      // EXITING).
      assert(p->state == Process::RUNNING || p->state == Process::WAITING);
      if (p->state == Process::RUNNING) {
        p->state = Process::INTERRUPTED;
      } else {
        p->state = Process::READY;
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
  assert(proc_process != NULL);
  swapcontext(&proc_process->uctx, &proc_uctx_running);
  legacy = false;
}


timeout ProcessManager::create_timeout(Process *process, double secs)
{
  assert(process != NULL);

  ev_tstamp tstamp;

  synchronized(timeouts) {
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
  synchronized(timeouts) {
    if (timeouts->size() == 0 || timeout.tstamp < timeouts->begin()->first) {
      // Need to interrupt the loop to update/set timer repeat.
      (*timeouts)[timeout.tstamp].push_back(timeout);
      update_timer = true;
      ev_async_send(loop, &async_watcher);
    } else {
      // Timer repeat is adequate, just add the timeout.
      assert(timeouts->size() >= 1);
      (*timeouts)[timeout.tstamp].push_back(timeout);
    }
  }
}


void ProcessManager::cancel_timeout(const timeout &timeout)
{
  synchronized(timeouts) {
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


void Clock::pause()
{
  initialize();

  synchronized(timeouts) {
    // For now, only one global clock (rather than clock per
    // process). This Means that we have to take special care to
    // ensure happens-before timing (currently done for local message
    // sends and spawning new processes, not currently done for
    // PROCESS_EXIT messages).
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

  synchronized(timeouts) {
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
  synchronized(timeouts) {
    if (clk != NULL) {
      clk->setElapsed(clk->getElapsed() + secs);

      // Might need to wakeup the processing thread.
      gate->open();
    }
  }
}


Process::Process()
{
  initialize();

  pthread_mutex_init(&m, NULL);

  refs = 0;

  current = NULL;

  generation = 0;

  /* Initialize the PID associated with the process. */
  if (!replaying) {
    /* Get a new unique pipe identifier. */
    pid.pipe = __sync_add_and_fetch(&global_pipe, 1);
  } else {
    /* Lookup pipe from record. */
    map<uint32_t, deque<uint32_t> >::iterator it = proc_process == NULL
      ? replay_pipes->find(0)
      : replay_pipes->find(proc_process->pid.pipe);

    /* Check that this is an expected process creation. */
    if (it == replay_pipes->end() && !it->second.empty())
      fatal("not expecting to create (this) process during replay");

    pid.pipe = it->second.front();
    it->second.pop_front();
  }

  if (recording) {
    assert(!replaying);
    record_pipes << " " << (proc_process == NULL ? 0 : proc_process->pid.pipe);
    record_pipes << " " << pid.pipe;
  }

  pid.ip = ip;
  pid.port = port;

  // If using a manual clock, try and set current time of process
  // using happens before relationship between creator and createe!
  synchronized(timeouts) {
    if (clk != NULL) {
      if (pthread_self() == proc_thread) {
        assert(proc_process != NULL);
        clk->setCurrent(this, clk->getCurrent(proc_process));
      } else {
        clk->setCurrent(this, clk->getCurrent());
      }
    }
  }
}


Process::~Process() {}


void Process::enqueue(struct msg *msg)
{
  assert(msg != NULL);

  // TODO(benh): Put filter inside lock statement below so that we can
  // guarantee the order of the messages seen by a filter are the same
  // as the order of messages seen by the process. This is hard to do
  // now because the locks aren't all correctly re-entrant. In
  // addition, we should really put the filterer in
  // ProcessManager::deliver because that updates the happens-before
  // timing relationship and if the message just gets filtered then
  // some timing could be incorrect.
  synchronized(filterer) {
    if (filterer != NULL) {
      if (filterer->filter(msg)) {
        free(msg);
        return;
      }
    }
  }

  lock();
  {
    assert(state != EXITED);

    msgs.push_back(msg);

    if (state == RECEIVING) {
      state = READY;
      process_manager->enqueue(this);
    } else if (state == AWAITING) {
      state = INTERRUPTED;
      process_manager->enqueue(this);
    }

    assert(state == INIT ||
           state == READY ||
           state == RUNNING ||
           state == PAUSED ||
           state == WAITING ||
           state == INTERRUPTED ||
           state == TIMEDOUT ||
           state == EXITING);
  }
  unlock();
}


struct msg * Process::dequeue()
{
  struct msg *msg = NULL;

  lock();
  {
    assert (state == RUNNING);
    if (!msgs.empty()) {
      msg = msgs.front();
      msgs.pop_front();
    }
  }
  unlock();

  return msg;
}


void Process::inject(const PID &from, MSGID id, const char *data, size_t length)
{
  if (replaying)
    return;

  if (!from)
    return;

  /* Disallow sending messages using an internal id. */
  if (id < PROCESS_MSGID)
    return;

  /* Allocate/Initialize outgoing message. */
  struct msg *msg = (struct msg *) malloc(sizeof(struct msg) + length);

  msg->from.pipe = from.pipe;
  msg->from.ip = from.ip;
  msg->from.port = from.port;
  msg->to.pipe = pid.pipe;
  msg->to.ip = pid.ip;
  msg->to.port = pid.port;
  msg->id = id;
  msg->len = length;

  if (length > 0)
    memcpy((char *) msg + sizeof(struct msg), data, length);

  // TODO(benh): Put filter inside lock statement below so that we can
  // guarantee the order of the messages seen by a filter are the same
  // as the order of messages seen by the process. This is hard to do
  // now because the locks aren't all correctly re-entrant. In
  // addition, we should really put the filterer in
  // ProcessManager::deliver because that updates the happens-before
  // timing relationship and if the message just gets filtered then
  // some timing could be incorrect.
  synchronized(filterer) {
    if (filterer != NULL) {
      if (filterer->filter(msg)) {
        free(msg);
        return;
      }
    }
  }

  lock();
  {
    msgs.push_front(msg);
  }
  unlock();
}


void Process::send(const PID &to, MSGID id, const char *data, size_t length)
{
  if (replaying)
    return;

  if (!to)
    return;
  
  /* Disallow sending messages using an internal id. */
  if (id < PROCESS_MSGID)
    return;

  /* Allocate/Initialize outgoing message. */
  struct msg *msg = (struct msg *) malloc(sizeof(struct msg) + length);

  msg->from.pipe = pid.pipe;
  msg->from.ip = pid.ip;
  msg->from.port = pid.port;
  msg->to.pipe = to.pipe;
  msg->to.ip = to.ip;
  msg->to.port = to.port;
  msg->id = id;
  msg->len = length;

  if (length > 0)
    memcpy((char *) msg + sizeof(struct msg), data, length);

  if (to.ip == ip && to.port == port)
    /* Local message. */
    process_manager->deliver(msg, this);
  else
    /* Remote message. */
    link_manager->send(msg);
}


MSGID Process::receive(double secs)
{
  // Free current message.
  if (current != NULL) {
    free(current);
    current = NULL;
  }

  // Check if there is a message queued.
  if ((current = dequeue()) != NULL)
    goto found;

  if (pthread_self() == proc_thread) {
    // Avoid blocking if negative seconds.
    if (secs >= 0)
      process_manager->receive(this, secs);

    // Check for a message (otherwise we timed out).
    if ((current = dequeue()) == NULL)
      goto timeout;

  } else {
    // TODO(benh): Handle timeout (this code actually waits
    // indefintely until the dequeue stops returning NULL.
    do {
      lock();
      {
	if (state == TIMEDOUT) {
	  state = RUNNING;
	  unlock();
	  goto timeout;
	}
	assert(state == RUNNING);
      }
      unlock();
      usleep(50000); // 50000 == ~RTT 
    } while ((current = dequeue()) == NULL);
  }

 found:
  assert (current != NULL);

  if (recording)
    process_manager->record(current);

  return current->id;

 timeout:
  current = (struct msg *) malloc(sizeof(struct msg));
  current->from.pipe = 0;
  current->from.ip = 0;
  current->from.port = 0;
  current->to.pipe = pid.pipe;
  current->to.ip = pid.ip;
  current->to.port = pid.port;
  current->id = PROCESS_TIMEOUT;
  current->len = 0;

  if (recording)
    process_manager->record(current);

  return current->id;
}


MSGID Process::serve(double secs, bool forever)
{
  do {
    switch (receive(secs)) {
      case PROCESS_DISPATCH: {
        void *pointer = (char *) current + sizeof(struct msg);
        std::tr1::function<void (void)> *delegator =
          *reinterpret_cast<std::tr1::function<void (void)> **>(pointer);
        (*delegator)();
        delete delegator;
        break;
      }

      default: {
        return msgid();
      }
    }
  } while (forever);
}


void Process::operator () ()
{
  serve();
}


const char * Process::body(size_t *length) const
{
  if (current != NULL && current->len > 0) {
    if (length != NULL)
      *length = current->len;
    return (char *) current + sizeof(struct msg);
  } else {
    if (length != NULL)
      *length = 0;
    return NULL;
  }
}


void Process::pause(double secs)
{
  if (pthread_self() == proc_thread) {
    if (replaying)
      process_manager->pause(this, 0);
    else
      process_manager->pause(this, secs);
  } else {
    sleep(secs);
  }
}


PID Process::link(const PID &to)
{
  if (!to)
    return to;

  process_manager->link(this, to);

  return to;
}


bool Process::await(int fd, int op, double secs, bool ignore)
{
  if (secs < 0)
    return true;

  /* TODO(benh): Handle invoking await from "outside" thread. */
  if (pthread_self() != proc_thread)
    fatal("unimplemented");

  return process_manager->await(this, fd, op, secs, ignore);
}


bool Process::ready(int fd, int op)
{
  if (fd < 0)
    return false;

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

  struct timeval timeout;
  memset(&timeout, 0, sizeof(timeout));

  select(fd+1, &rdset, &wrset, NULL, &timeout);

  return FD_ISSET(fd, &rdset) || FD_ISSET(fd, &wrset);
}


double Process::elapsed()
{
  double now = 0;

  synchronized(timeouts) {
    if (clk != NULL) {
      now = clk->getCurrent(this);
    } else {
      // TODO(benh): Unclear if want ev_now(...) or ev_time().
      now = ev_time();
    }
  }

  return now;
}


PID Process::spawn(Process *process)
{
  initialize();

  if (process != NULL) {
    // If using a manual clock, try and set current time of process
    // using happens before relationship between spawner and spawnee!
    synchronized(timeouts) {
      if (clk != NULL) {
        if (pthread_self() == proc_thread) {
          assert(proc_process != NULL);
          clk->setCurrent(process, clk->getCurrent(proc_process));
        } else {
          clk->setCurrent(process, clk->getCurrent());
        }
      }
    }

    process_manager->spawn(process);

    return process->self();
  } else {
    return PID();
  }
}


bool Process::wait(const PID &pid)
{
  initialize();

  if (!pid)
    return false;

  // N.B. This could result in a deadlock! We could check if such was
  // the case by doing:
  //
  //   if (proc_process && proc_process->pid == pid) {
  //     handle deadlock here;
  //   }
  //
  // But for now, deadlocks seem like better bugs to try and fix than
  // segmentation faults that might occur because a client thinks it
  // has waited on a process and it is now finished (and can be
  // cleaned up).

  if (pthread_self() != proc_thread)
    return process_manager->external_wait(pid);
  else
    return process_manager->wait(proc_process, pid);
}


void Process::invoke(const function<void (void)> &thunk)
{
  initialize();
  ProcessManager::invoke(thunk);
}


void Process::filter(Filter *filter)
{
  initialize();

  synchronized(filterer) {
    filterer = filter;
  }
}


void Process::post(const PID &to, MSGID id, const char *data, size_t length)
{
  initialize();

  if (replaying)
    return;

  if (!to)
    return;

  /* Disallow sending messages using an internal id. */
  if (id < PROCESS_MSGID)
    return;

  /* Allocate/Initialize outgoing message. */
  struct msg *msg = (struct msg *) malloc(sizeof(struct msg) + length);

  msg->from.pipe = 0;
  msg->from.ip = 0;
  msg->from.port = 0;
  msg->to.pipe = to.pipe;
  msg->to.ip = to.ip;
  msg->to.port = to.port;
  msg->id = id;
  msg->len = length;

  if (length > 0)
    memcpy((char *) msg + sizeof(struct msg), data, length);

  if (to.ip == ip && to.port == port)
    /* Local message. */
    process_manager->deliver(msg);
  else
    /* Remote message. */
    link_manager->send(msg);
}


void Process::dispatcher(Process *process, function<void (void)> *delegator)
{
  if (replaying)
    return;

  if (process == NULL)
    return;

  /* Allocate/Initialize outgoing message. */
  struct msg *msg = (struct msg *) malloc(sizeof(struct msg) + sizeof(delegator));

  msg->from.pipe = 0;
  msg->from.ip = 0;
  msg->from.port = 0;
  msg->to.pipe = process->pid.pipe;
  msg->to.ip = process->pid.ip;
  msg->to.port = process->pid.port;
  msg->id = PROCESS_DISPATCH;
  msg->len = sizeof(delegator);

  memcpy((char *) msg + sizeof(struct msg), &delegator, sizeof(delegator));

  process_manager->deliver(msg);
}
