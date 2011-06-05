#ifndef PROCESS_HPP
#define PROCESS_HPP

#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

#include <sys/time.h>

#include <queue>

#include <tr1/functional>

#include "pid.hpp"


typedef uint16_t MSGID;


const MSGID PROCESS_ERROR = 0;
const MSGID PROCESS_TIMEOUT = 1;
const MSGID PROCESS_EXIT = 2;
const MSGID PROCESS_MSGID = PROCESS_EXIT+1;


struct msg
{
  PID from;
  PID to;
  MSGID id;
  uint32_t len;
};


class ProcessClock {
public:
  static void pause();
  static void resume();
  static void advance(double secs);
};


class MessageFilter {
public:
  virtual bool filter(struct msg *) = 0;
};


class Process {
public:
  /* Returns pid of process; valid even before calling spawn. */
  PID getPID() const;

  /* Sends a message to PID without a return address. */
  static void post(const PID &to, MSGID id);

  /* Sends a message with data to PID without a return address. */
  static void post(const PID &to, MSGID id, const char *data, size_t length);

  /* Spawn a new process. */
  static PID spawn(Process *process);

  /* Wait for PID to exit (returns true if actually waited on a process). */
  static bool wait(const PID &pid);

  /* Wait for PID to exit (returns true if actually waited on a process). */
  static bool wait(Process *process);

  /* Invoke the thunk in a legacy safe way. */
  static void invoke(const std::tr1::function<void (void)> &thunk);

  /* Filter messages to be enqueued (except for timeout messages). */
  static void filter(MessageFilter *);

protected:
  Process();
  virtual ~Process();

  /* Function run when process spawned. */
  virtual void operator() () = 0;

  /* Returns the PID describing this process. */
  PID self() const;

  /* Returns the sender's PID of the last dequeued (current) message. */
  PID from() const;

  /* Returns the id of the current message. */
  MSGID msgid() const;

  /* Returns pointer and length of body of last dequeued (current) message. */
  const char * body(size_t *length) const;

  /* Put a message at front of queue (will not reschedule process). */
  virtual void inject(const PID &from, MSGID id, const char *data, size_t length);

  /* Sends a message to PID. */
  virtual void send(const PID &to , MSGID);

  /* Sends a message with data to PID. */
  virtual void send(const PID &to, MSGID id, const char *data, size_t length);

  /* Blocks for message indefinitely. */
  virtual MSGID receive();

  /* Blocks for message at most specified seconds. */
  virtual MSGID receive(double secs);

  /* Sends a message to PID and then blocks for a message indefinitely. */
  virtual MSGID call(const PID &to , MSGID id);

  /* Sends a message with data to PID and then blocks for a message. */
  virtual MSGID call(const PID &to, MSGID id, const char *data, size_t length);

  /* Sends, and then blocks for a message at most specified seconds. */
  virtual MSGID call(const PID &to, MSGID id, const char *data, size_t length, double secs);

  /* Blocks at least specified seconds (may block longer). */
  virtual void pause(double secs);

  /* Links with the specified PID. */
  virtual PID link(const PID &pid);

  /* IO events for awaiting. */
  enum { RDONLY = 01, WRONLY = 02, RDWR = 03 };

  /* Wait until operation is ready for file descriptor (or message received). */
  virtual bool await(int fd, int op, const timeval& tv);

  /* Wait until operation is ready for file descriptor (or message received if not ignored). */
  virtual bool await(int fd, int op, const timeval& tv, bool ignore);

  /* Returns true if operation on file descriptor is ready. */
  virtual bool ready(int fd, int op);

  /* Returns sub-second elapsed time (according to this process). */
  double elapsed();

private:
  friend class LinkManager;
  friend class ProcessManager;
  friend class ProcessReference;
  friend void * schedule(void *arg);

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
         EXITING,
	 EXITED } state;

  /* Active references. */
  int refs;

  /* Queue of received messages. */
  std::deque<struct msg *> msgs;

  /* Current message. */
  struct msg *current;

  /* Current "blocking" generation. */
  int generation;

  /* Process PID. */
  PID pid;

  /* Continuation/Context of process. */
  ucontext_t uctx;

  /* Lock/mutex protecting internals. */
  pthread_mutex_t m;
  void lock() { pthread_mutex_lock(&m); }
  void unlock() { pthread_mutex_unlock(&m); }

  /* Enqueues the specified message. */
  void enqueue(struct msg *msg);

  /* Dequeues a message or returns NULL. */
  struct msg * dequeue();
};


inline MSGID Process::msgid() const
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


inline PID Process::getPID() const
{
  return self();
}


inline void Process::post(const PID &to, MSGID id)
{
  post(to, id, NULL, 0);
}


#endif /* PROCESS_HPP */
