#include <signal.h>

#include <cerrno>
#include <iostream>
#include <string>
#include <sstream>

#include <process.hpp>

#include <boost/bind.hpp>
#include <boost/unordered_map.hpp>

#include "nexus_exec.h"

#include "fatal.hpp"
#include "lock.hpp"
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
  friend class nexus::NexusExecutorDriver;
  
protected:
  PID slave;
  NexusExecutorDriver* driver;
  Executor* executor;
  FrameworkID fid;
  SlaveID sid;

public:
  ExecutorProcess(const PID& _slave,
                  NexusExecutorDriver* _driver,
                  Executor* _executor,
                  FrameworkID _fid)
    : slave(_slave), driver(_driver), executor(_executor), fid(_fid) {}

protected:
  void operator() ()
  {
    link(slave);
    send(slave, pack<E2S_REGISTER_EXECUTOR>(fid));
    while(true) {
      // TODO(benh): Is there a better way to architect this code? In
      // particular, if the executor blocks in a callback, we can't
      // process any other messages. This is especially tricky if a
      // slave dies since we won't handle the PROCESS_EXIT message in
      // a timely manner (if at all).
      switch(receive()) {
        case S2E_REGISTER_REPLY: {
          string name;
          string args;
          unpack<S2E_REGISTER_REPLY>(sid, name, args);
          ExecutorArgs execArg(sid, fid, name, args);
          invoke(bind(&Executor::init, executor, driver, ref(execArg)));
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
          invoke(bind(&Executor::launchTask, executor, driver, ref(task)));
          break;
        }

        case S2E_KILL_TASK: {
          TaskID tid;
          unpack<S2E_KILL_TASK>(tid);
          invoke(bind(&Executor::killTask, executor, driver, tid));
          break;
        }

        case S2E_FRAMEWORK_MESSAGE: {
          FrameworkMessage msg;
          unpack<S2E_FRAMEWORK_MESSAGE>(msg);
          invoke(bind(&Executor::frameworkMessage, executor, driver, ref(msg)));
          break;
        }

        case S2E_KILL_EXECUTOR: {
          invoke(bind(&Executor::shutdown, executor, driver));
          exit(0);
        }

        case PROCESS_EXIT: {
          // TODO: Pass an argument to shutdown to tell it this is abnormal?
          invoke(bind(&Executor::shutdown, executor, driver));

	  // This is a pretty bad state ... no slave is left. Rather
	  // than exit lets kill our process group (which includes
	  // ourself) hoping to clean up any processes this executor
	  // launched itself.
	  // TODO(benh): Maybe do a SIGTERM and then later do a SIGKILL?
	  killpg(0, SIGKILL);
        }

        default: {
          // TODO: Is this serious enough to exit?
          cerr << "Received unknown message ID " << msgid()
               << " from " << from() << endl;
          break;
        }
      }
    }
  }
};

}} /* namespace nexus { namespace internal { */


/*
 * Implementation of C++ API.
 */


// Default implementation of error() that logs to stderr and exits
void Executor::error(ExecutorDriver*, int code, const string &message)
{
  cerr << "Nexus error: " << message
       << " (error code: " << code << ")" << endl;
  // TODO(*): Don't exit here, let errors be recoverable. (?)
  exit(1);
}


NexusExecutorDriver::NexusExecutorDriver(Executor* _executor)
  : executor(_executor)
{
  // Create mutex and condition variable
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
}


NexusExecutorDriver::~NexusExecutorDriver()
{
  pthread_mutex_destroy(&mutex);
}


int NexusExecutorDriver::run()
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

  process = new ExecutorProcess(slave, this, executor, fid);

  Process::wait(Process::spawn(process));

  process = NULL;

  return 0;
}


int NexusExecutorDriver::sendStatusUpdate(const TaskStatus &status)
{
  Lock lock(&mutex);

  /* TODO(benh): Increment ref count on process. */
  
  if (!process) {
    //executor->error(this, EINVAL, "Executor has exited");
    return -1;
  }

  process->send(process->slave,
                process->pack<E2S_STATUS_UPDATE>(process->fid,
                                                 status.taskId,
                                                 status.state,
                                                 status.data));

  /* TODO(benh): Decrement ref count on process. */

  return 0;
}


int NexusExecutorDriver::sendFrameworkMessage(const FrameworkMessage &message)
{
  Lock lock(&mutex);

  /* TODO(benh): Increment ref count on process. */
  
  if (!process) {
    //executor->error(this, EINVAL, "Executor has exited");
    return -1;
  }

  process->send(process->slave,
                process->pack<E2S_FRAMEWORK_MESSAGE>(process->fid,
                                                     message));

  /* TODO(benh): Decrement ref count on process. */

  return 0;
}


/*
 * Implementation of C API.
 */


namespace nexus { namespace internal {

/*
 * We wrap calls from the C API into the C++ API with the following
 * specialized implementation of Executor.
 */
class CExecutor : public Executor {
public:
  nexus_exec* exec;
  ExecutorDriver* driver; // Set externally after object is created
  
  CExecutor(nexus_exec* _exec) : exec(_exec), driver(NULL) {}

  virtual ~CExecutor() {}

  virtual void init(ExecutorDriver*, const ExecutorArgs& args)
  {
    exec->init(exec,
               args.slaveId.c_str(),
               args.frameworkId.c_str(),
               args.frameworkName.c_str(),
               args.data.data(),
               args.data.size());
  }

  virtual void launchTask(ExecutorDriver*, const TaskDescription& task)
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
    exec->launch_task(exec, &td);
  }

  virtual void killTask(ExecutorDriver*, TaskID taskId)
  {
    exec->kill_task(exec, taskId);
  }
  
  virtual void frameworkMessage(ExecutorDriver*,
                                const FrameworkMessage& message)
  {
    nexus_framework_message msg = { message.slaveId.c_str(),
                                    message.taskId,
                                    message.data.data(),
                                    message.data.size() };
    exec->framework_message(exec, &msg);
  }
  
  virtual void shutdown(ExecutorDriver*)
  {
    exec->shutdown(exec);
  }
  
  virtual void error(ExecutorDriver*, int code, const std::string& message)
  {
    exec->error(exec, code, message.c_str());
  }
};


/*
 * A single CExecutor instance used with the C API.
 *
 * TODO: Is this a good idea? How can one unit-test C frameworks? It might
 *       be better to have a hashtable as in the scheduler API eventually.
 */
CExecutor* c_executor = NULL;

}} /* namespace nexus { namespace internal {*/


extern "C" {


int nexus_exec_run(struct nexus_exec* exec)
{
  if (exec == NULL || c_executor != NULL) {
    errno = EINVAL;
    return -1;
  }

  CExecutor executor(exec);
  c_executor = &executor;
  
  NexusExecutorDriver driver(&executor);
  executor.driver = &driver;
  driver.run();

  c_executor = NULL;

  return 0;
}


int nexus_exec_send_message(struct nexus_exec* exec,
                            struct nexus_framework_message* msg)
{
  if (exec == NULL || c_executor == NULL || msg == NULL) {
    errno = EINVAL;
    return -1;
  }

  string data((char*) msg->data, msg->data_len);
  FrameworkMessage message(string(msg->sid), msg->tid, data);

  c_executor->driver->sendFrameworkMessage(message);

  return 0;
}


int nexus_exec_status_update(struct nexus_exec* exec,
                             struct nexus_task_status* status)
{

  if (exec == NULL || c_executor == NULL || status == NULL) {
    errno = EINVAL;
    return -1;
  }

  string data((char*) status->data, status->data_len);
  TaskStatus ts(status->tid, status->state, data);

  c_executor->driver->sendStatusUpdate(ts);

  return 0;
}

} /* extern "C" */
