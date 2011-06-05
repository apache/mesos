#include <cerrno>
#include <iostream>
#include <string>
#include <sstream>

#include <process.hpp>

#include <boost/bind.hpp>
#include <boost/unordered_map.hpp>

#include "nexus_exec.h"

#include "fatal.hpp"
#include "messages.hpp"
#include "nexus_exec.hpp"

using std::cerr;
using std::endl;
using std::string;

using boost::bind;
using boost::ref;
using boost::unordered_map;

using namespace nexus;
using namespace nexus::internal;

namespace nexus { namespace internal {

class ExecutorProcess : public Tuple<Process>
{
public:
  friend class nexus::Executor;
  
protected:
  PID slave;
  FrameworkID fid;
  SlaveID sid;
  Executor* executor;

public:
  ExecutorProcess(const PID& _slave, FrameworkID _fid, Executor* _executor)
    : slave(_slave), fid(_fid), executor(_executor) {}

protected:
  void operator() ()
  {
    link(slave);
    send(slave, pack<E2S_REGISTER_EXECUTOR>(fid));
    while(true) {
      switch(receive()) {
        case S2E_REGISTER_REPLY: {
          string name;
          string args;
          unpack<S2E_REGISTER_REPLY>(sid, name, args);
          ExecutorArgs ea(sid, fid, name, args);
          invoke(bind(&Executor::init, executor, ea));
          break;
        }

        case S2E_RUN_TASK: {
          TaskID tid;
          string name;
          string args;
          Params params;
          unpack<S2E_RUN_TASK>(tid, name, args, params);
          TaskDescription task(tid, sid, name, params.getMap(), args);
          send(slave, pack<E2S_STATUS_UPDATE>(fid, tid, TASK_RUNNING, ""));
          invoke(bind(&Executor::startTask, executor, ref(task)));
          break;
        }

        case S2E_KILL_TASK: {
          TaskID tid;
          unpack<S2E_KILL_TASK>(tid);
          invoke(bind(&Executor::killTask, executor, tid));
          break;
        }

        case S2E_FRAMEWORK_MESSAGE: {
          FrameworkMessage message;
          unpack<S2E_FRAMEWORK_MESSAGE>(message);
          invoke(bind(&Executor::frameworkMessage, executor, ref(message)));
          break;
        }

        case S2E_KILL_EXECUTOR: {
          invoke(bind(&Executor::shutdown, executor));
          return; // Shut down this libpocess process
        }

        case PROCESS_EXIT: {
          exit(1);
        }

        default: {
          cerr << "Received unknown message ID " << msgid()
               << " from " << from() << endl;
          break;
        }
      }
    }
  }
};

}} /* namespace nexus { namespace internal { */


static ExecutorProcess* _process = NULL;


namespace {

// RAII class for locking mutexes
struct Lock
{
  pthread_mutex_t* m;
  Lock(pthread_mutex_t* _m): m(_m) { pthread_mutex_lock(m); }
  ~Lock() { pthread_mutex_unlock(m); }
};

} /* namespace { */


/*
 * Implementation of C++ API.
 */


Executor::Executor()
{
  // Create mutex and condition variable
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
}


Executor::~Executor()
{
  pthread_mutex_destroy(&mutex);
}


void Executor::run()
{
  // Set stream buffering mode to flush on newlines so that we capture logs
  // from user processes even when output is redirected to a file.
  setvbuf(stdout, 0, _IOLBF, 0);
  setvbuf(stderr, 0, _IOLBF, 0);

  PID slave;
  FrameworkID fid;

  char* value;
  std::istringstream iss;

  /* Get slave PID from environment. */
  value = getenv("NEXUS_SLAVE_PID");

  if (value == NULL)
    fatal("expecting NEXUS_SLAVE_PID in environment");

  iss.str(value);

  if (!(iss >> slave))
    fatal("cannot parse NEXUS_SLAVE_PID");

  /* Get framework ID from environment. */
  value = getenv("NEXUS_FRAMEWORK_ID");

  if (value == NULL)
    fatal("expecting NEXUS_FRAMEWORK_ID in environment");

  iss.clear();
  iss.str(value);

  if (!(iss >> fid))
    fatal("cannot parse NEXUS_FRAMEWORK_ID");

  _process = new ExecutorProcess(slave, fid, this);

  Process::wait(Process::spawn(_process));

  _process = NULL;
}


void Executor::sendStatusUpdate(const TaskStatus &status)
{
  Lock lock(&mutex);

  /* TODO(benh): Increment ref count on process. */
  
  if (!_process)
    error(EINVAL, "Executor has exited");

  _process->send(_process->slave,
                 _process->pack<E2S_STATUS_UPDATE>(_process->fid,
                                                   status.taskId,
                                                   status.state,
                                                   status.data));

  /* TODO(benh): Decrement ref count on process. */
}


void Executor::sendFrameworkMessage(const FrameworkMessage &message)
{
  Lock lock(&mutex);

  /* TODO(benh): Increment ref count on process. */
  
  if (!_process)
    error(EINVAL, "Executor has exited");

  _process->send(_process->slave,
                 _process->pack<E2S_FRAMEWORK_MESSAGE>(_process->fid,
                                                       message));

  /* TODO(benh): Decrement ref count on process. */
}


// Default implementation of error() that logs to stderr and exits
void Executor::error(int code, const string &message)
{
  cerr << "Nexus error: " << message
       << " (error code: " << code << ")" << endl;
  // TODO(*): Don't exit here, let errors be recoverable.
  exit(1);
}


// Default implementation of shutdown() that just exits
void Executor::shutdown()
{
  exit(0);
}


/*
 * Implementation of C API.
 */


namespace nexus {

/*
 * We wrap calls from the C API into the C++ API with the following
 * specialized implementation of Executor.
 */
class CExecutor : public Executor {
private:
  nexus_exec* exec;
  
public:
  CExecutor(nexus_exec* _exec) : exec(_exec) {}

  virtual ~CExecutor() {}

  virtual void init(const ExecutorArgs& args)
  {
    exec->init(exec,
               args.slaveId.c_str(),
               args.frameworkId,
               args.frameworkName.c_str(),
               args.data.data(),
               args.data.size());
  }

  virtual void startTask(const TaskDescription& task)
  {
    // Convert params to key=value list
    Params paramsObj(task.params);
    string paramsStr = paramsObj.str();
    nexus_task_desc td = { task.taskId,
                           task.slaveId.c_str(),
                           task.name.c_str(),
                           paramsStr.c_str(),
                           task.arg.data(),
                           task.arg.size() };
    exec->run(exec, &td);
  }

  virtual void killTask(TaskID taskId)
  {
    exec->kill(exec, taskId);
  }
  
  virtual void frameworkMessage(const FrameworkMessage& message)
  {
    nexus_framework_message msg = { message.slaveId.c_str(),
                                    message.taskId,
                                    message.data.data(),
                                    message.data.size() };
    exec->message(exec, &msg);
  }
  
  virtual void shutdown()
  {
    exec->shutdown(exec);
  }
  
  virtual void error(int code, const std::string& message)
  {
    exec->error(exec, code, message.c_str());
  }
};

} /* namespace nexus { */


/*
 * A single CExecutor instance used with the C API.
 */
CExecutor* _executor = NULL;


extern "C" {


int nexus_exec_run(struct nexus_exec* exec)
{
  if (exec == NULL || _executor != NULL) {
    errno = EINVAL;
    return -1;
  }

  _executor = new CExecutor(exec);
  
  _executor->run();

  _executor = NULL;

  return 0;
}


int nexus_exec_send_message(struct nexus_exec* exec,
                            struct nexus_framework_message* msg)
{
  if (exec == NULL || _executor == NULL || msg == NULL) {
    errno = EINVAL;
    return -1;
  }

  string data((char*) msg->data, msg->data_len);
  FrameworkMessage message(string(msg->sid), msg->tid, data);

  _executor->sendFrameworkMessage(message);

  return 0;
}


int nexus_exec_status_update(struct nexus_exec* exec,
                             struct nexus_task_status* status)
{

  if (exec == NULL || _executor == NULL || status == NULL) {
    errno = EINVAL;
    return -1;
  }

  string data((char*) status->data, status->data_len);
  TaskStatus ts(status->tid, status->state, data);

  _executor->sendStatusUpdate(ts);

  return 0;
}

} /* extern "C" */
