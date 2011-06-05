#include <signal.h>

#include <cerrno>
#include <iostream>
#include <string>
#include <sstream>

#include <mesos_exec.hpp>
#include <process.hpp>

#include <boost/bind.hpp>
#include <boost/unordered_map.hpp>

#include "common/fatal.hpp"
#include "common/lock.hpp"
#include "common/logging.hpp"
#include "common/type_utils.hpp"

#include "messaging/messages.hpp"

using std::cerr;
using std::endl;
using std::string;

using boost::bind;
using boost::cref;
using boost::unordered_map;

using namespace mesos;
using namespace mesos::internal;


namespace mesos { namespace internal {

class ExecutorProcess : public MesosProcess
{
public:
  ExecutorProcess(const PID& _slave, MesosExecutorDriver* _driver,
                  Executor* _executor, const FrameworkID& _frameworkId,
                  bool _local)
    : slave(_slave), driver(_driver), executor(_executor),
      frameworkId(_frameworkId), local(_local), terminate(false) {}

  ~ExecutorProcess() {}

protected:
  virtual void operator () ()
  {
    link(slave);

    // Register with slave.
    Message<E2S_REGISTER_EXECUTOR> out;
    *out.mutable_framework_id() = frameworkId;
    send(slave, out);

    while(true) {
      // TODO(benh): Is there a better way to architect this code? In
      // particular, if the executor blocks in a callback, we can't
      // process any other messages. This is especially tricky if a
      // slave dies since we won't handle the PROCESS_EXIT message in
      // a timely manner (if at all).

      // Check for terminate in the same way as SchedulerProcess. See
      // comments there for an explanation of why this is necessary.
      if (terminate)
        return;

      switch(receive(2)) {
        case S2E_REGISTER_REPLY: {
          const Message<S2E_REGISTER_REPLY>& msg = message();
          slaveId = msg.args().slave_id();
          invoke(bind(&Executor::init, executor, driver, cref(msg.args())));
          break;
        }

        case S2E_RUN_TASK: {
          const Message<S2E_RUN_TASK>& msg = message();

          const TaskDescription& task = msg.task();

          Message<E2S_STATUS_UPDATE> out;
          *out.mutable_framework_id() = frameworkId;
          TaskStatus* status = out.mutable_status();
          *status->mutable_task_id() = task.task_id();
          *status->mutable_slave_id() = slaveId;
          status->set_state(TASK_RUNNING);
          send(slave, out);

          invoke(bind(&Executor::launchTask, executor, driver, cref(task)));
          break;
        }

        case S2E_KILL_TASK: {
          const Message<S2E_KILL_TASK>& msg = message();
          invoke(bind(&Executor::killTask, executor, driver,
                      cref(msg.task_id())));
          break;
        }

        case S2E_FRAMEWORK_MESSAGE: {
          const Message<S2E_FRAMEWORK_MESSAGE>& msg = message();
          const FrameworkMessage& message = msg.message();
          invoke(bind(&Executor::frameworkMessage, executor, driver,
                      cref(message)));
          break;
        }

        case S2E_KILL_EXECUTOR: {
          invoke(bind(&Executor::shutdown, executor, driver));
          if (!local)
            exit(0);
          else
            return;
        }

        case PROCESS_EXIT: {
          // TODO: Pass an argument to shutdown to tell it this is abnormal?
          invoke(bind(&Executor::shutdown, executor, driver));

          // This is a pretty bad state ... no slave is left. Rather
          // than exit lets kill our process group (which includes
          // ourself) hoping to clean up any processes this executor
          // launched itself.
          // TODO(benh): Maybe do a SIGTERM and then later do a SIGKILL?
          if (!local)
            killpg(0, SIGKILL);
          else
            return;
        }

        case PROCESS_TIMEOUT: {
          break;
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

private:
  friend class mesos::MesosExecutorDriver;

  PID slave;
  MesosExecutorDriver* driver;
  Executor* executor;
  FrameworkID frameworkId;
  SlaveID slaveId;
  bool local;

  volatile bool terminate;
};

}} /* namespace mesos { namespace internal { */


/*
 * Implementation of C++ API.
 */


// Default implementation of error() that logs to stderr and exits
void Executor::error(ExecutorDriver* driver, int code, const string &message)
{
  cerr << "Mesos error: " << message
       << " (error code: " << code << ")" << endl;
  driver->stop();
}


MesosExecutorDriver::MesosExecutorDriver(Executor* _executor)
  : executor(_executor), running(false)
{
  // Create mutex and condition variable
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&cond, 0);
}


MesosExecutorDriver::~MesosExecutorDriver()
{
  Process::wait(process->self());
  delete process;

  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);
}


int MesosExecutorDriver::start()
{
  Lock lock(&mutex);

  if (running) {
    return -1;
  }

  // Set stream buffering mode to flush on newlines so that we capture logs
  // from user processes even when output is redirected to a file.
  setvbuf(stdout, 0, _IOLBF, 0);
  setvbuf(stderr, 0, _IOLBF, 0);

  bool local;

  PID slave;
  FrameworkID frameworkId;

  char* value;
  std::istringstream iss;

  /* Check if this is local (for example, for testing). */
  value = getenv("MESOS_LOCAL");

  if (value != NULL)
    local = true;
  else
    local = false;

  /* Get slave PID from environment. */
  value = getenv("MESOS_SLAVE_PID");

  if (value == NULL)
    fatal("expecting MESOS_SLAVE_PID in environment");

  slave = PID(value);

  if (!slave)
    fatal("cannot parse MESOS_SLAVE_PID");

  /* Get framework ID from environment. */
  value = getenv("MESOS_FRAMEWORK_ID");

  if (value == NULL)
    fatal("expecting MESOS_FRAMEWORK_ID in environment");

  frameworkId.set_value(value);

  process = new ExecutorProcess(slave, this, executor, frameworkId, local);

  Process::spawn(process);

  running = true;

  return 0;
}


int MesosExecutorDriver::stop()
{
  Lock lock(&mutex);

  if (!running) {
    return -1;
  }

  process->terminate = true;

  running = false;

  pthread_cond_signal(&cond);

  return 0;
}


int MesosExecutorDriver::join()
{
  Lock lock(&mutex);
  while (running)
    pthread_cond_wait(&cond, &mutex);

  return 0;
}


int MesosExecutorDriver::run()
{
  int ret = start();
  return ret != 0 ? ret : join();
}


int MesosExecutorDriver::sendStatusUpdate(const TaskStatus& status)
{
  Lock lock(&mutex);

  if (!running) {
    //executor->error(this, EINVAL, "Executor has exited");
    return -1;
  }

  Message<E2S_STATUS_UPDATE> out;
  *out.mutable_framework_id() = process->frameworkId;
  *out.mutable_status() = status;
  process->send(process->slave, out);

  return 0;
}


int MesosExecutorDriver::sendFrameworkMessage(const FrameworkMessage& message)
{
  Lock lock(&mutex);

  if (!running) {
    //executor->error(this, EINVAL, "Executor has exited");
    return -1;
  }

  Message<E2S_FRAMEWORK_MESSAGE> out;
  *out.mutable_framework_id() = process->frameworkId;
  *out.mutable_message() = message;
  *out.mutable_message()->mutable_slave_id() = process->slaveId;
  process->send(process->slave, out);

  return 0;
}
