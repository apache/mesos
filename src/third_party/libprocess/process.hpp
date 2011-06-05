#ifndef PROCESS_HPP
#define PROCESS_HPP

#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

#ifdef USE_LITHE
#include <lithe.hh>

#include <ht/ht.h>
#include <ht/spinlock.h>
#endif /* USE_LITHE */

#include <iostream>
#include <map>
#include <queue>
#include <string>
#include <utility>

#include <tr1/functional>

typedef uint16_t MSGID;

const MSGID PROCESS_ERROR = 0;
const MSGID PROCESS_TIMEOUT = 1;
const MSGID PROCESS_EXIT = 2;
const MSGID PROCESS_MSGID = PROCESS_EXIT+1;

typedef struct PID
{
  uint32_t pipe;
  uint32_t ip;
  uint16_t port;

  operator std::string() const;
  bool operator ! () const;
} PID;

std::ostream& operator << (std::ostream& stream, const PID& pid);
std::istream& operator >> (std::istream& stream, PID& pid);

bool operator < (const PID &left, const PID &right);
bool operator == (const PID &left, const PID &right);

PID make_pid(const char *);

struct msg
{
  PID from;
  PID to;
  MSGID id;
  uint32_t len;
};


#ifdef USE_LITHE

using lithe::Scheduler;

class ProcessScheduler : public Scheduler
{
private:
  int lock;
  int waiter;
  lithe_task_t task;
  std::map<lithe_sched_t *, std::pair<int, int> > children;

protected:
  void enter();
  void yield(lithe_sched_t *child);
  void reg(lithe_sched_t *child);
  void unreg(lithe_sched_t *child);
  void request(lithe_sched_t *child, int k);
  void unblock(lithe_task_t *task);

  void schedule();

public:
  ProcessScheduler();
  ~ProcessScheduler();
};

#else

void * schedule(void *arg);

#endif /* USE_LITHE */


class Process {
private:
  friend class LinkManager;
  friend class ProcessManager;
#ifdef USE_LITHE
  friend class ProcessScheduler;
#else
  friend void * schedule(void *arg);
#endif /* USE_LITHE */

  /* Flag indicating state of process. */
  enum { INIT,
	 READY,
	 RUNNING,
	 RECEIVING,
	 PAUSED,
	 AWAITING,
	 WAITING,
	 INTERRUPTED,
	 TIMEDOUT,
	 EXITED } state;

  /* Queue of messages received. */
  std::queue<struct msg *> msgs;

  /* Current message. */
  struct msg *current;

  /* Current "blocking" generation. */
  int generation;

  /* Process PID. */
  PID pid;

#ifdef USE_LITHE
  lithe_task_t task;
#endif /* USE_LITHE */

  /* Continuation/Context of process. */
  ucontext_t uctx;

  /* Lock/mutex protecting internals. */
#ifdef USE_LITHE
  int l;
  void lock() { spinlock_lock(&l); }
  void unlock() { spinlock_unlock(&l); }
#else
  pthread_mutex_t m;
  void lock() { pthread_mutex_lock(&m); }
  void unlock() { pthread_mutex_unlock(&m); }
#endif /* USE_LITHE */

  /* Enqueues the specified message. */
  void enqueue(struct msg *msg);

  /* Dequeues a message or returns NULL. */
  struct msg * dequeue();

#if defined(SWIGPYTHON) || defined(SWIGRUBY)
public:
#else
protected:
#endif /* SWIG */

  Process();

  /* Function run when process spawned. */
  virtual void operator() () = 0;

  /* Returns the PID describing this process. */
  PID self();

  /* Returns the sender's PID of the last dequeued (current) message. */
  PID from();

  MSGID msgid();

  /* Sends a message to PID. */
  void send(const PID &, MSGID);

  /* Sends a message with data to PID. */
  void send(const PID &, MSGID, const char *data, size_t length);

  /* Blocks for message indefinitely. */
  MSGID receive();

  /* Blocks for message at most specified seconds. */
  MSGID receive(double);

  /* Sends a message to PID and then blocks for a message indefinitely. */
  MSGID call(const PID &, MSGID);

  /* Sends a message with data to PID and then blocks for a message. */
  MSGID call(const PID &, MSGID, const char *data, size_t length);

  /* Sends, and then blocks for a message at most specified seconds. */
  MSGID call(const PID &, MSGID, const char *data, size_t length, double);

  /* Returns pointer and length of body of last dequeued (current) message. */
  const char * body(size_t *length);

  /* Blocks at least specified seconds (may block longer). */
  void pause(double);

  /* Links with the specified PID. */
  PID link(const PID &);

  /* IO operations for awaiting. */
  enum { RDONLY = 01, WRONLY = 02, RDWR = 03 };

  /* Wait until operation is ready for file descriptor (or message received). */
  bool await(int fd, int op, const timeval& tv);

  /* Wait until operation is ready for file descriptor (or message received if not ignored). */
  bool await(int fd, int op, const timeval& tv, bool ignore);

  /* Returns true if operation on file descriptor is ready. */
  bool ready(int fd, int op);

public:
  virtual ~Process();

  /* Returns pid of process; valid even before calling spawn. */
  PID getPID();

  /* Sends a message to PID without a return address. */
  static void post(const PID &, MSGID);

  /* Sends a message with data to PID without a return address. */
  static void post(const PID &, MSGID, const char *data, size_t length);

  /* Spawn a new process. */
  static PID spawn(Process *process);

  /* Wait for PID to exit (returns true if actually waited on a process). */
  static bool wait(PID pid);

  /* Wait for PID to exit (returns true if actually waited on a process). */
  static bool wait(Process *process);

  /* Invoke the thunk in a legacy safe way. */
  static void invoke(const std::tr1::function<void (void)> &thunk);
};


inline MSGID Process::msgid()
{
  return current != NULL ? current->id : PROCESS_ERROR;
}


inline void Process::send(const PID &to, MSGID id)
{
  send(to, id, NULL, 0);
}


inline MSGID Process::call(const PID &to, MSGID id)
{
  return call(to, id, NULL, 0);
}


inline MSGID Process::call(const PID &to, MSGID id,
			   const char *data, size_t length)
{
  return call(to, id, data, length, 0);
}


inline MSGID Process::receive()
{
  return receive(0);
}


inline PID Process::getPID()
{
  return self();
}


inline void Process::post(const PID &to, MSGID id)
{
  post(to, id, NULL, 0);
}


#endif /* PROCESS_HPP */
