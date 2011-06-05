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
#include "decoder.hpp"
#include "encoder.hpp"
#include "fatal.hpp"
#include "foreach.hpp"
#include "gate.hpp"
#include "process.hpp"
#include "synchronized.hpp"

using boost::tuple;

using std::deque;
using std::find;
using std::list;
using std::map;
using std::max;
using std::ostream;
using std::queue;
using std::set;
using std::stack;
using std::string;
using std::stringstream;

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

  void link(Process* process, const PID& to);

  void send(Message* message);
  void send(DataEncoder* encoder, int s);

  DataEncoder* next(int s);

  void closed(int s);

  void exited(const Node& node);
  void exited(Process* process);

private:
  /* Map from PID (local/remote) to process. */
  map<PID, set<Process*> > links;

  /* Map from socket to node (ip, port). */
  map<int, Node> sockets;

  /* Maps from node (ip, port) to socket. */
  map<Node, int> temps;
  map<Node, int> persists;

  set<int> disposables;

  /* Map from socket to outgoing messages. */
  map<int, queue<DataEncoder*> > outgoing;

  /* Protects instance variables. */
  synchronizable(this);
};


class ProcessManager
{
public:
  ProcessManager();
  ~ProcessManager();

  ProcessReference use(const PID &pid);

  void deliver(Message *message, Process *sender = NULL);

  PID spawn(Process *process);
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
  map<string, Process *> processes;
  synchronizable(processes);

  /* Waiting processes (protected by synchronizable(processes)). */
  map<Process *, set<Process *> > waiters;

  /* Map of gates for waiting threads. */
  map<Process *, Gate *> gates;

  /* Queue of runnable processes (implemented as deque). */
  deque<Process *> runq;
  synchronizable(runq);
};


class ProxyManager : public Process
{
public:
  ProxyManager() {}
  ~ProxyManager() {}

  void manage(Proxy* proxy)
  {
    assert(proxy != NULL);
    proxies[proxy->self()] = proxy;
    link(proxy->self());
  }

protected:
  virtual void operator () ()
  {
    while (true) {
      serve();
      if (name() == EXIT && proxies.count(from()) > 0) {
        Proxy* proxy = proxies[from()];
        proxies.erase(from());
        delete proxy;
      }
    }
  }

private:
  map<PID, Proxy*> proxies;
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

/* Unique id that can be assigned to each process. */
static uint32_t id = 0;

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

/**
 * Filter. Synchronized support for using the filterer needs to be
 * recursive incase a filterer wants to do anything fancy (which is
 * possible and likely given that filters will get used for testing).
*/
static Filter *filterer = NULL;
static synchronizable(filterer) = SYNCHRONIZED_INITIALIZER_RECURSIVE;


/**
 * Instance of ProxyManager.
 */
static ProxyManager* proxy_manager = NULL;


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


Message* encode(const PID &from, const PID &to, const string &name, const string &data = "")
{
  Message* message = new Message();
  message->from = from;
  message->to = to;
  message->name = name;
  message->body = data;
  return message;
}


void transport(Message* message, Process* sender = NULL)
{
  if (message->to.ip == ip && message->to.port == port) {
    // Local message.
    process_manager->deliver(message, sender);
  } else {
    // Remote message.
    link_manager->send(message);
  }
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

  foreach (const timeout &timeout, timedout) {
    process_manager->timedout(timeout.pid, timeout.generation);
  }
}


void handle_await(struct ev_loop *loop, ev_io *watcher, int revents)
{
  tuple<PID, int> *t = (tuple<PID, int> *) watcher->data;
  process_manager->awaited(t->get<0>(), t->get<1>());
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
      // Interrupted, try again.
      continue;
    } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Might block, try again later.
      break;
    } else if (length <= 0) {
      // Socket error ... we consider closed.
      link_manager->closed(c);
      delete decoder;
      ev_io_stop(loop, watcher);
      delete watcher;
      break;
    } else {
      assert(length > 0);

      // Decode as much of the data as possible.
      const deque<Message*>& messages = decoder->decode(data, length);

      if (!messages.empty()) {
        foreach (Message* message, messages) {
          process_manager->deliver(message);
        }
      } else if (messages.empty() && decoder->failed()) {
        link_manager->closed(c);
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
    assert(size > 0);

    ssize_t length = send(c, data, size, MSG_NOSIGNAL);

    if (length < 0 && (errno == EINTR)) {
      // Interrupted, try again.
      continue;
    } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Might block, try again later.
      break;
    } else if (length <= 0) {
      // Socket closed or error ... we consider closed.
      link_manager->closed(c);
      delete encoder;
      ev_io_stop(loop, watcher);
      delete watcher;
      break;
    } else {
      assert(length > 0);

      // Update the encoder with the amount sent.
      encoder->backup(size - length);

      // See if there is any more of the message to send.
      if (encoder->remaining() == 0) {
        delete encoder;

        // Check for more stuff to send on socket.
        encoder = link_manager->next(c);
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
    link_manager->closed(c);
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
    link_manager->closed(c);
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
    DataDecoder* decoder = new DataDecoder(c);

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
    synchronized (stacks) {
      stacks->push(recyclable);
    }
    recyclable = NULL;
  }

  do {
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

  if (pthread_create(&io_thread, NULL, serve, loop) != 0)
    fatalerror("failed to initialize node (pthread_create)");

  initializing = false;
}


Proxy::Proxy(int _c) : c(_c)
{
  // TODO(benh): Do proper initialization of the proxy manager. Right
  // now we can rely on invocations of the Proxy constructor to be
  // serial, so we don't need to do any fancy
  // synchronization. However, ultimately, we'd like to stick the
  // initialization into 'initialize' above, but that seemed to have
  // some issues. :(
  if (proxy_manager == NULL) {
    proxy_manager = new ProxyManager();
    Process::spawn(proxy_manager);
  }

  dispatch(proxy_manager, &ProxyManager::manage, this);
}


Proxy::~Proxy() {}


void Proxy::operator () ()
{
  receive(10);

  if (name() != TIMEOUT) {
    size_t length;
    const char* data = body(&length);
    link_manager->send(new HttpResponseEncoder(std::string(data, length)), c);
  } else {
    link_manager->send(new HttpGatewayTimeoutEncoder(), c);
  }

  // How the socket gets closed is a bit esoteric. :( If the browser
  // closes their socket then we will close it in when
  // LinkManger::closed gets called. Otherwise, after one of the above
  // messages get's sent then LinkManager::next will get called, which
  // will close the socket. Ultimately, the ProxyManager will deal
  // with deallocating the Proxy, even though it was allocated in the
  // decoder. Ugh, gross, gross, gross. :(
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
  // EXIT message gets generated. This functionality has only
  // been implemented when the link is local, not remote. Of course,
  // if there is nobody listening on the remote side, then this should
  // work remotely ... but if there is someone listening remotely just
  // not at that id, then it will silently continue executing.

  assert(process != NULL);

  const Node node = { to.ip, to.port };

  synchronized (this) {
    // Check if node is remote and there isn't a persistant link.
    if ((node.ip != ip || node.port != port) && persists.count(node) == 0) {
      // Okay, no link, lets create a socket.
      int s;

      if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)
        fatalerror("failed to link (socket)");

      if (set_nbio(s) < 0)
        fatalerror("failed to link (set_nbio)");

      sockets[s] = node;
      persists[node] = s;

      sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = PF_INET;
      addr.sin_port = htons(to.port);
      addr.sin_addr.s_addr = to.ip;

      // Allocate and initialize the decoder and watcher.
      DataDecoder* decoder = new DataDecoder(s);

      ev_io *watcher = new ev_io();
      watcher->data = decoder;

      // Try and connect to the node using this socket.
      if (connect(s, (sockaddr *) &addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS)
          fatalerror("failed to link (connect)");

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


void LinkManager::send(Message* message)
{
  assert(message != NULL);

  DataEncoder* encoder = new MessageEncoder(message);

  Node node = { message->to.ip, message->to.port };

  synchronized (this) {
    // Check if there is already a link.
    if (persists.count(node) > 0 || temps.count(node) > 0) {
      int s = persists.count(node) > 0 ? persists[node] : temps[node];

      // Check whether or not this socket has an outgoing queue.
      if (outgoing.count(s) != 0) {
        outgoing[s].push(encoder);
      } else {
        // Must be a persistant socket since temporary socket
        // shouldn't outlast it's outgoing queue!
        assert(persists.count(node) != 0);
        assert(temps.count(node) == 0 || temps[node] != s);

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
    } else {
      // No peristant or temporary socket to the node currently
      // exists, so we create a temporary one.
      int s;

      if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)
        fatalerror("failed to send (socket)");

      if (set_nbio(s) < 0)
        fatalerror("failed to send (set_nbio)");

      sockets[s] = node;
      temps[node] = s;

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
        if (errno != EINPROGRESS)
          fatalerror("failed to send (connect)");

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


void LinkManager::send(DataEncoder* encoder, int s)
{
  assert(encoder != NULL);

  // TODO(benh): This is a big hack for sending data on a socket from
  // an encoder where we want one-time (i.e., disposable) use of the
  // socket. This is particularly useful right now for proxies we set
  // up for HTTP GET requests, however, the semantics of what this
  // send means should be less esoteric.

  synchronized (this) {
    assert(sockets.count(s) == 0);
    assert(outgoing.count(s) == 0);

    sockets[s] = Node(); // TODO(benh): HACK!
    outgoing[s];
    disposables.insert(s);

    // Allocate and initialize the watcher.
    ev_io *watcher = new ev_io();
    watcher->data = encoder;

    ev_io_init(watcher, send_data, s, EV_WRITE);

    synchronized (watchers) {
      watchers->push(watcher);
    }

    ev_async_send(loop, &async_watcher);
  }
}


DataEncoder* LinkManager::next(int s)
{
  DataEncoder* encoder = NULL;

  // Sometimes we look for another encoder even though this socket
  // isn't actually maintained by the LinkManager (e.g., for proxy
  // sockets). In the future this kind of esoteric semantics will
  // hopefully get improved. :(
  synchronized (this) {
    if (sockets.count(s) > 0 && outgoing.count(s) > 0) {
      if (!outgoing[s].empty()) {
        // More messages!
        encoder = outgoing[s].front();
        outgoing[s].pop();
      } else {
        // No more messages ... erase the outgoing queue.
        outgoing.erase(s);

        // Close the socket if it was for one-time use.
        if (disposables.count(s) > 0) {
          disposables.erase(s);
          sockets.erase(s);
          close(s);
        }

        // Close the socket if it was temporary.
        const Node &node = sockets[s];
        if (temps.count(node) > 0 && temps[node] == s) {
          temps.erase(node);
          sockets.erase(s);
          close(s);
        }
      }
    }
  }

  return encoder;
}


void LinkManager::closed(int s)
{
  synchronized (this) {
    if (sockets.count(s) > 0) {
      const Node& node = sockets[s];

      // Don't bother invoking exited unless socket was persistant.
      if (persists.count(node) > 0 && persists[node] == s) {
	persists.erase(node);
	exited(node);
      } else {
        if (temps.count(node) > 0 && temps[node] == s) {
          temps.erase(node);
        }
      }

      sockets.erase(s);
      outgoing.erase(s);
    }
  }

  // This might have just been a receiving socket, so we want to make
  // sure to call close so that the file descriptor can get reused.
  close(s);
}


void LinkManager::exited(const Node &node)
{
  // TODO(benh): It would be cleaner if this routine could call back
  // into ProcessManager ... then we wouldn't have to convince
  // ourselves that the accesses to each Process object will always be
  // valid.
  synchronized (this) {
    list<PID> removed;
    // Look up all linked processes.
    foreachpair (const PID &pid, set<Process *> &processes, links) {
      if (pid.ip == node.ip && pid.port == node.port) {
        // N.B. If we call exited(pid) we might invalidate iteration.
        foreach (Process *process, processes) {
          Message* message = encode(pid, process->pid, EXIT);
          process->enqueue(message);
        }
        removed.push_back(pid);
      }
    }

    foreach (const PID &pid, removed) {
      links.erase(pid);
    }
  }
}


void LinkManager::exited(Process *process)
{
  synchronized (this) {
    /* Remove any links this process might have had. */
    foreachpair (_, set<Process *> &processes, links)
      processes.erase(process);

    const PID &pid = process->self();

    /* Look up all linked processes. */
    map<PID, set<Process *> >::iterator it = links.find(pid);

    if (it != links.end()) {
      set<Process *> &processes = it->second;
      foreach (Process *p, processes) {
        assert(process != p);
        Message *message = encode(pid, p->pid, EXIT);
        // TODO(benh): Preserve happens-before when using clock.
        p->enqueue(message);
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


void ProcessManager::deliver(Message *message, Process *sender)
{
  assert(message != NULL);

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
          clk->setCurrent(receiver, max(clk->getCurrent(receiver),
                                        clk->getCurrent(sender)));
        }
      }
    }

    receiver->enqueue(message);
  } else {
    delete message;
  }
}


PID ProcessManager::spawn(Process *process)
{
  assert(process != NULL);

  process->state = Process::INIT;

  synchronized (processes) {
    if (processes.count(process->pid.id) > 0) {
      return PID();
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

  return process->self();
}



void ProcessManager::link(Process *process, const PID &to)
{
  // Check if the pid is local.
  if (!(to.ip == ip && to.port == port)) {
    link_manager->link(process, to);
  } else {
    // Since the pid is local we want to get a reference to it's
    // underlying process so that while we are invoking the link
    // manager we don't miss sending a possible EXIT.
    if (ProcessReference _ = use(to)) {
      link_manager->link(process, to);
    } else {
      // Since the pid isn't valid it's process must have already died
      // (or hasn't been spawned yet) so send a process exit message.
      Message *message = encode(to, process->pid, EXIT);
      process->enqueue(message);
    }
  }
}


void ProcessManager::receive(Process *process, double secs)
{
  assert(process != NULL);
  process->lock();
  {
    /* Ensure nothing enqueued since check in Process::receive. */
    if (process->messages.empty()) {
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
  synchronized (processes) {
    if (processes.count(pid.id) > 0) {
      assert(processes[pid.id]->state != Process::EXITED);
      waiters[processes[pid.id]].insert(process);
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
  synchronized (processes) {
    if (processes.count(pid.id) > 0) {
      Process *process = processes[pid.id];
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
    if (!ignore && !process->messages.empty()) {
      process->unlock();
      return false;
    }

    // Treat an await with a bad fd as an interruptible pause!
    if (fd >= 0) {
      /* Allocate/Initialize the watcher. */
      ev_io *watcher = new ev_io();

      if ((op & Process::RDWR) == Process::RDWR)
        ev_io_init(watcher, handle_await, fd, EV_READ | EV_WRITE);
      else if ((op & Process::RDONLY) == Process::RDONLY)
        ev_io_init(watcher, handle_await, fd, EV_READ);
      else if ((op & Process::WRONLY) == Process::WRONLY)
        ev_io_init(watcher, handle_await, fd, EV_WRITE);

      // Tuple describing state (on heap in case we can't "cancel" it,
      // the watcher will always fire, even if we get interrupted and
      // return early, so this tuple will get cleaned up when the
      // watcher runs).
      watcher->data = new tuple<PID, int>(process->pid, process->generation);

      /* Enqueue the watcher. */
      synchronized (watchers) {
        watchers->push(watcher);
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
  synchronized (runq) {
    assert(find(runq.begin(), runq.end(), process) == runq.end());
    runq.push_back(process);
  }
    
  /* Wake up the processing thread if necessary. */
  gate->open();
}


Process * ProcessManager::dequeue()
{
  Process *process = NULL;

  synchronized (runq) {
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
    std::cerr << "libprocess: " << process->pid
              << " exited due to "
              << e.what() << std::endl;
  } catch (...) {
    std::cerr << "libprocess: " << process->pid
              << " exited due to unknown exception" << std::endl;
  }

  cleanup(process);
}


void ProcessManager::cleanup(Process *process)
{
  // Processes that were waiting on exiting process.
  list<Process *> resumable;

  // Possible gate non-libprocess threads are waiting at.
  Gate *gate = NULL;

  // Stop new process references from being created.
  process->state = Process::EXITING;

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

      // Free current message.
      if (process->current) {
        delete process->current;
      }

      processes.erase(process->pid.id);

      // Confirm that the process is not in any waiting queue.
      foreachpair (_, set<Process *> &waiting, waiters) {
        assert(waiting.find(process) == waiting.end());
      }

      // Grab all the waiting processes that are now resumable.
      foreach (Process *waiter, waiters[process]) {
        resumable.push_back(waiter);
      }

      waiters.erase(process);

      // Lookup gate to wake up waiting non-libprocess threads.
      map<Process *, Gate *>::iterator it = gates.find(process);
      if (it != gates.end()) {
        gate = it->second;
        // N.B. The last thread that leaves the gate also free's it.
        gates.erase(it);
      }
        
      assert(process->refs == 0);
      process->state = Process::EXITED;
    }
    process->unlock();
  }

  // Inform link manager.
  link_manager->exited(process);

  // Confirm process not in runq.
  synchronized (runq) {
    assert(find(runq.begin(), runq.end(), process) == runq.end());
  }

  // N.B. After opening the gate we can no longer dereference
  // 'process' since it might already be cleaned up by user code (a
  // waiter might have cleaned up the stack where the process was
  // allocated).
  if (gate != NULL) {
    gate->open();
  }

  // And resume all processes waiting too.
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
      assert(timeouts->size() >= 1);
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


void Clock::pause()
{
  initialize();

  synchronized (timeouts) {
    // For now, only one global clock (rather than clock per
    // process). This Means that we have to take special care to
    // ensure happens-before timing (currently done for local message
    // sends and spawning new processes, not currently done for
    // EXIT messages).
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


Process::Process(const std::string& _id)
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
        assert(proc_process != NULL);
        clk->setCurrent(this, clk->getCurrent(proc_process));
      } else {
        clk->setCurrent(this, clk->getCurrent());
      }
    }
  }
}


Process::~Process() {}


void Process::enqueue(Message *message)
{
  assert(message != NULL);

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

  // if the message is an acknowledgement, just record the acknowledgement and move on


  lock();
  {
    assert(state != EXITED);

    messages.push_back(message);

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


Message * Process::dequeue()
{
  Message *message = NULL;

  lock();
  {
    assert (state == RUNNING);
    if (!messages.empty()) {
      message = messages.front();
      messages.pop_front();
    }
  }
  unlock();

  return message;
}


void Process::inject(const PID &from, const string &name, const char *data, size_t length)
{
  if (!from)
    return;

  // Encode outgoing message.
  Message *message = encode(from, pid, name, string(data, length));

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

  lock();
  {
    messages.push_front(message);
  }
  unlock();
}


void Process::send(const PID &to, const string &name, const char *data, size_t length)
{
  if (!to) {
    return;
  }

  // Encode and transport outgoing message.
  transport(encode(pid, to, name, string(data, length)), this);
}


string Process::receive(double secs)
{
  // Free current message.
  if (current != NULL) {
    delete current;
    current = NULL;
  }

  // Check if there is a message queued.
  if ((current = dequeue()) != NULL) {
    goto found;
  }

  if (pthread_self() == proc_thread) {
    // Avoid blocking if negative seconds.
    if (secs >= 0) {
      process_manager->receive(this, secs);
    }

    // Check for a message (otherwise we timed out).
    if ((current = dequeue()) == NULL) {
      goto timeout;
    }

  } else {
    // TODO(benh): Handle timeout (this code actually waits
    // indefinitely until the dequeue stops returning NULL.
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
  assert(current != NULL);
  return name();

 timeout:
  assert(current == NULL);
  current = encode(PID(), pid, TIMEOUT);
  return name();
}


string Process::serve(double secs, bool forever)
{
  do {
    const string &name = receive(secs);
    if (name == DISPATCH) {
      void *pointer = (char *) current->body.data();
      std::tr1::function<void (void)> *delegator =
        *reinterpret_cast<std::tr1::function<void (void)> **>(pointer);
      (*delegator)();
      delete delegator;
    } else {
      return name;
    }
  } while (forever);
}


void Process::operator () ()
{
  serve();
}


PID Process::from() const
{
  if (current != NULL) {
    return current->from;
  } else {
    return PID();
  }
}


string Process::name() const
{
  if (current != NULL) {
    return current->name;
  } else {
    return ERROR;
  }
}


const char * Process::body(size_t *length) const
{
  if (current != NULL && current->body.size() > 0) {
    if (length != NULL) {
      *length = current->body.size();
    }
    return (char *) current->body.c_str();
  } else {
    if (length != NULL) {
      *length = 0;
    }
    return NULL;
  }
}


void Process::pause(double secs)
{
  if (pthread_self() == proc_thread) {
  } else {
    sleep(secs);
  }
}


PID Process::link(const PID &to)
{
  if (!to) {
    return to;
  }

  process_manager->link(this, to);

  return to;
}


bool Process::await(int fd, int op, double secs, bool ignore)
{
  if (secs < 0) {
    return true;
  }

  /* TODO(benh): Handle invoking await from "outside" thread. */
  if (pthread_self() != proc_thread)
    fatal("unimplemented");

  return process_manager->await(this, fd, op, secs, ignore);
}


bool Process::ready(int fd, int op)
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


double Process::elapsed()
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


PID Process::spawn(Process *process)
{
  initialize();

  if (process != NULL) {
    // If using a manual clock, try and set current time of process
    // using happens before relationship between spawner and spawnee!
    synchronized (timeouts) {
      if (clk != NULL) {
        if (pthread_self() == proc_thread) {
          assert(proc_process != NULL);
          clk->setCurrent(process, clk->getCurrent(proc_process));
        } else {
          clk->setCurrent(process, clk->getCurrent());
        }
      }
    }

    return process_manager->spawn(process);
  } else {
    return PID();
  }
}


bool Process::wait(const PID &pid)
{
  initialize();

  if (!pid) {
    return false;
  }

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

  if (pthread_self() != proc_thread) {
    return process_manager->external_wait(pid);
  }

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

  synchronized (filterer) {
    filterer = filter;
  }
}


void Process::post(const PID &to, const string &name, const char *data, size_t length)
{
  initialize();

  if (!to) {
    return;
  }

  // Encode and transport outgoing message.
  transport(encode(PID(), to, name, string(data, length)));
}


void Process::dispatcher(Process *process, function<void (void)> *delegator)
{
  if (process == NULL) {
    return;
  }

  // Encode and deliver outgoing message.
  Message *message = encode(PID(), process->pid, DISPATCH, 
                            string((char *) &delegator, sizeof(delegator)));

  // TODO(benh): Maintain happens before using proc_process?
  process_manager->deliver(message);
}
