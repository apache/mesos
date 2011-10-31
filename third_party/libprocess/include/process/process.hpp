#ifndef __PROCESS_PROCESS_HPP__
#define __PROCESS_PROCESS_HPP__

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

#include <sys/time.h>

#include <map>
#include <queue>

#include <tr1/functional>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>


namespace process {

const std::string NOTHING = "__process_nothing__";
const std::string ERROR = "__process_error__";
const std::string TIMEOUT = "__process_timeout__";
const std::string EXITED = "__process_exited__";
const std::string TERMINATE = "__process_terminate__";


struct Message {
  std::string name;
  UPID from;
  UPID to;
  std::string body;
};


class Clock {
public:
  static double now();
  static void pause();
  static void resume();
  static void advance(double secs);
};


class Filter {
public:
  // TODO(benh): Support filtering HTTP requests?
  virtual bool filter(Message*) = 0;
};


class ProcessBase
{
public:
  ProcessBase(const std::string& id = "");

  virtual ~ProcessBase();

  UPID self() const { return pid; }

  static UPID spawn(ProcessBase* process, bool manage = false);

protected:
  // Function run when process spawned.
//   virtual void operator () () = 0;
  virtual void operator () ()
  {
    do { if (serve() == TERMINATE) break; } while (true);
  }

  // Returns the sender's PID of the last dequeued (current) message.
  UPID from() const;

  // Returns the name of the last dequeued (current) message.
  const std::string& name() const;

  // Returns the body of the last dequeued (current) message.
  const std::string& body() const;

  // Put a message at front of queue.
  void inject(const UPID& from,
              const std::string& name,
              const char* data = NULL,
              size_t length = 0);

  // Sends a message with data to PID.
  void send(const UPID& to,
            const std::string &name,
            const char *data = NULL,
            size_t length = 0);

  // Blocks for message at most specified seconds (0 implies forever).
  std::string receive(double secs = 0);

  // Processes dispatch messages.
  std::string serve(double secs = 0, bool once = false);

  // Blocks at least specified seconds (may block longer).
  void pause(double secs);

  // Links with the specified PID.
  UPID link(const UPID& pid);

  // IO events for polling.
  enum { RDONLY = 01, WRONLY = 02, RDWR = 03 };

  // Wait until operation is ready for file descriptor (or message
  // received ignore is false).
  bool poll(int fd, int op, double secs = 0, bool ignore = true);

  // Returns true if operation on file descriptor is ready.
  bool ready(int fd, int op);

  // Returns sub-second elapsed time (according to this process).
  double elapsedTime();

  // Delegate incoming message's with the specified name to pid.
  void delegate(const std::string& name, const UPID& pid)
  {
    delegates[name] = pid;
  }

  typedef std::tr1::function<void()> MessageHandler;

  // Install a handler for a message.
  void installMessageHandler(
      const std::string& name,
      const MessageHandler& handler)
  {
    messageHandlers[name] = handler;
  }

  template <typename T>
  void installMessageHandler(
      const std::string& name,
      void (T::*method)())
  {
    MessageHandler handler = std::tr1::bind(method, dynamic_cast<T*>(this));
    installMessageHandler(name, handler);
  }

  typedef std::tr1::function<Promise<HttpResponse>(const HttpRequest&)>
    HttpRequestHandler;

  // Install a handler for an HTTP request.
  void installHttpHandler(
      const std::string& name,
      const HttpRequestHandler& handler)
  {
    httpHandlers[name] = handler;
  }

  template <typename T>
  void installHttpHandler(
      const std::string& name,
      Promise<HttpResponse> (T::*method)(const HttpRequest&))
  {
    HttpRequestHandler handler =
      std::tr1::bind(method, dynamic_cast<T*>(this),
                     std::tr1::placeholders::_1);
    installHttpHandler(name, handler);
  }

private:
  friend class SocketManager;
  friend class ProcessManager;
  friend class ProcessReference;
  friend void* schedule(void *);

  // Process states.
  enum { INIT,
	 READY,
	 RUNNING,
	 RECEIVING,
	 SERVING,
	 PAUSED,
	 POLLING,
	 WAITING,
	 INTERRUPTED,
	 TIMEDOUT,
         FINISHING,
	 FINISHED } state;

  // Lock/mutex protecting internals.
  pthread_mutex_t m;
  void lock() { pthread_mutex_lock(&m); }
  void unlock() { pthread_mutex_unlock(&m); }

  // Enqueue the specified message, request, or dispatcher.
  void enqueue(Message* message, bool inject = false);
  void enqueue(std::pair<HttpRequest*, Promise<HttpResponse>*>* request);
  void enqueue(std::tr1::function<void(ProcessBase*)>* dispatcher);

  // Dequeue a message, request, or dispatcher, or returns NULL.
  template <typename T> T* dequeue();

  // Queue of received messages.
  std::deque<Message*> messages;

  // Queue of HTTP requests (with the promise used for responses).
  std::deque<std::pair<HttpRequest*, Promise<HttpResponse>*>*> requests;

  // Queue of dispatchers.
  std::deque<std::tr1::function<void(ProcessBase*)>*> dispatchers;

  // Delegates for messages.
  std::map<std::string, UPID> delegates;

  // Handlers for messages.
  std::map<std::string, MessageHandler> messageHandlers;

  // Handlers for HTTP requests.
  std::map<std::string, HttpRequestHandler> httpHandlers;

  // Current message.
  Message* current;

  // Active references.
  int refs;

  // Current "blocking" generation.
  int generation;

  // Process PID.
  UPID pid;

  // Continuation/Context of process.
  ucontext_t uctx;
};


template <typename T>
class Process : public virtual ProcessBase {
public:
  Process(const std::string& id = "") : ProcessBase(id) {}

  // Returns pid of process; valid even before calling spawn.
  PID<T> self() const { return PID<T>(dynamic_cast<const T*>(this)); }
};


/**
 * Initialize the library.
 *
 * @param initialize_google_logging whether or not to initialize the
 *        Google Logging library (glog). If the application is also
 *        using glog, this should be set to false.
 */
void initialize(bool initialize_google_logging = true);


/**
 * Spawn a new process.
 *
 * @param process process to be spawned
 * @param manage boolean whether process should get garbage collected
 */
template <typename T>
PID<T> spawn(T* t, bool manage = false)
{
  if (!ProcessBase::spawn(t, manage)) {
    return PID<T>();
  }

  return PID<T>(t);
}

template <typename T>
PID<T> spawn(T& t, bool manage = false)
{
  return spawn(&t, manage);
}


/**
 * Send a TERMINATE message to a process, injecting the message ahead
 * of all other messages queued up for that process if requested. Note
 * that currently terminate only works for local processes (in the
 * future we plan to make this more explicit via the use of a PID
 * instead of a UPID).
 *
 * @param inject if true message will be put on front of messae queue
 */
void terminate(const UPID& pid, bool inject = true);
void terminate(const ProcessBase& process, bool inject = true);
void terminate(const ProcessBase* process, bool inject = true);


/**
 * Wait for process to exit no more than specified seconds (returns
 * true if actually waited on a process).
 *
 * @param PID id of the process
 * @param secs max time to wait, 0 implies wait for ever
 */
bool wait(const UPID& pid, double secs = 0);
bool wait(const ProcessBase& process, double secs = 0);
bool wait(const ProcessBase* process, double secs = 0);


/**
 * Invoke the thunk in a legacy safe way (i.e., outside of libprocess).
 *
 * @param thunk function to be invoked
 */
void invoke(const std::tr1::function<void(void)>& thunk);


/**
 * Use the specified filter on messages that get enqueued (note,
 * however, that you cannot filter timeout messages).
 *
 * @param filter message filter
 */
void filter(Filter* filter);


/**
 * Sends a message with data without a return address.
 *
 * @param to receiver
 * @param name message name
 * @param data data to send (gets copied)
 * @param length length of data
 */
void post(const UPID& to,
          const std::string& name,
          const char* data = NULL,
          size_t length = 0);


// Inline implementations of above.
inline void terminate(const ProcessBase& process, bool inject)
{
  terminate(process.self(), inject);
}


inline void terminate(const ProcessBase* process, bool inject)
{
  terminate(process->self(), inject);
}


inline bool wait(const ProcessBase& process, double secs)
{
  return wait(process.self(), secs);
}


inline bool wait(const ProcessBase* process, double secs)
{
  return wait(process->self(), secs);
}

} // namespace process {

#endif // __PROCESS_PROCESS_HPP__
