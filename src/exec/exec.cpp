#include <signal.h>

#include <glog/logging.h>

#include <iostream>
#include <string>
#include <sstream>

#include <boost/bind.hpp>

#include <mesos/executor.hpp>

#include <process/dispatch.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include "common/fatal.hpp"
#include "common/lock.hpp"
#include "common/logging.hpp"
#include "common/type_utils.hpp"
#include "common/uuid.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;

using namespace process;

using boost::bind;
using boost::cref;

using std::string;

using process::wait; // Necessary on some OS's to disambiguate.


namespace mesos { namespace internal {

class ExecutorProcess : public ProtobufProcess<ExecutorProcess>
{
public:
  ExecutorProcess(const UPID& _slave,
                  MesosExecutorDriver* _driver,
                  Executor* _executor,
                  const FrameworkID& _frameworkId,
                  const ExecutorID& _executorId,
                  bool _local)
    : slave(_slave),
      driver(_driver),
      executor(_executor),
      frameworkId(_frameworkId),
      executorId(_executorId),
      local(_local)
  {
    installProtobufHandler<ExecutorRegisteredMessage>(
        &ExecutorProcess::registered,
        &ExecutorRegisteredMessage::args);

    installProtobufHandler<RunTaskMessage>(
        &ExecutorProcess::runTask,
        &RunTaskMessage::task);

    installProtobufHandler<KillTaskMessage>(
        &ExecutorProcess::killTask,
        &KillTaskMessage::task_id);

    installProtobufHandler<FrameworkToExecutorMessage>(
        &ExecutorProcess::frameworkMessage,
        &FrameworkToExecutorMessage::slave_id,
        &FrameworkToExecutorMessage::framework_id,
        &FrameworkToExecutorMessage::executor_id,
        &FrameworkToExecutorMessage::data);

    installProtobufHandler<ShutdownExecutorMessage>(
        &ExecutorProcess::shutdown);

    installMessageHandler(EXITED, &ExecutorProcess::exited);
  }

  virtual ~ExecutorProcess() {}

protected:
  virtual void operator () ()
  {
    VLOG(1) << "Executor started at: " << self();

    link(slave);

    // Register with slave.
    RegisterExecutorMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    send(slave, message);

    do { if (serve() == TERMINATE) break; } while (true);
  }

  void registered(const ExecutorArgs& args)
  {
    VLOG(1) << "Executor registered on slave " << args.slave_id();
    slaveId = args.slave_id();
    invoke(bind(&Executor::init, executor, driver, cref(args)));
  }

  void runTask(const TaskDescription& task)
  {
    VLOG(1) << "Executor asked to run task '" << task.task_id() << "'";

    invoke(bind(&Executor::launchTask, executor, driver, cref(task)));
  }

  void killTask(const TaskID& taskId)
  {
    VLOG(1) << "Executor asked to kill task '" << taskId << "'";
    invoke(bind(&Executor::killTask, executor, driver, cref(taskId)));
  }

  void frameworkMessage(const SlaveID& slaveId,
			const FrameworkID& frameworkId,
			const ExecutorID& executorId,
			const string& data)
  {
    VLOG(1) << "Executor received framework message";
    invoke(bind(&Executor::frameworkMessage, executor, driver, cref(data)));
  }

  void shutdown()
  {
    VLOG(1) << "Executor asked to shutdown";
    // TODO(benh): Any need to invoke driver.stop?
    invoke(bind(&Executor::shutdown, executor, driver));
    if (!local) {
      exit(0);
    } else {
      terminate(this);
    }
  }

  void exited()
  {
    VLOG(1) << "Slave exited, trying to shutdown";

    // TODO: Pass an argument to shutdown to tell it this is abnormal?
    invoke(bind(&Executor::shutdown, executor, driver));

    // This is a pretty bad state ... no slave is left. Rather
    // than exit lets kill our process group (which includes
    // ourself) hoping to clean up any processes this executor
    // launched itself.
    // TODO(benh): Maybe do a SIGTERM and then later do a SIGKILL?
    if (!local) {
      killpg(0, SIGKILL);
    } else {
      terminate(this);
    }
  }

  void sendStatusUpdate(const TaskStatus& status)
  {
    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(frameworkId);
    update->mutable_executor_id()->MergeFrom(executorId);
    update->mutable_slave_id()->MergeFrom(slaveId);
    update->mutable_status()->MergeFrom(status);
    update->set_timestamp(elapsedTime());
    update->set_uuid(UUID::random().toBytes());
    send(slave, message);
  }

  void sendFrameworkMessage(const string& data)
  {
    ExecutorToFrameworkMessage message;
    message.mutable_slave_id()->MergeFrom(slaveId);
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    message.set_data(data);
    send(slave, message);
  }

private:
  friend class mesos::MesosExecutorDriver;

  UPID slave;
  MesosExecutorDriver* driver;
  Executor* executor;
  FrameworkID frameworkId;
  ExecutorID executorId;
  SlaveID slaveId;
  bool local;
};

}} // namespace mesos { namespace internal {


// Implementation of C++ API.


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

  // TODO(benh): Initialize glog.

  // Initialize libprocess library (but not glog, done above).
  process::initialize(false);
}


MesosExecutorDriver::~MesosExecutorDriver()
{
  // Just as in SchedulerProcess, we might wait here indefinitely if
  // MesosExecutorDriver::stop has not been invoked.
  wait(process);
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

  UPID slave;
  FrameworkID frameworkId;
  ExecutorID executorId;

  char* value;
  std::istringstream iss;

  /* Check if this is local (for example, for testing). */
  value = getenv("MESOS_LOCAL");

  if (value != NULL) {
    local = true;
  } else {
    local = false;
  }

  /* Get slave PID from environment. */
  value = getenv("MESOS_SLAVE_PID");

  if (value == NULL) {
    fatal("expecting MESOS_SLAVE_PID in environment");
  }

  slave = UPID(value);

  if (!slave) {
    fatal("cannot parse MESOS_SLAVE_PID");
  }

  /* Get framework ID from environment. */
  value = getenv("MESOS_FRAMEWORK_ID");

  if (value == NULL) {
    fatal("expecting MESOS_FRAMEWORK_ID in environment");
  }

  frameworkId.set_value(value);

  /* Get executor ID from environment. */
  value = getenv("MESOS_EXECUTOR_ID");

  if (value == NULL) {
    fatal("expecting MESOS_EXECUTOR_ID in environment");
  }

  executorId.set_value(value);

  process =
    new ExecutorProcess(slave, this, executor, frameworkId, executorId, local);

  spawn(process);

  running = true;

  return 0;
}


int MesosExecutorDriver::stop()
{
  Lock lock(&mutex);

  if (!running) {
    return -1;
  }

  CHECK(process != NULL);
  terminate(process);
  running = false;
  pthread_cond_signal(&cond);

  return 0;
}


int MesosExecutorDriver::join()
{
  Lock lock(&mutex);

  while (running) {
    pthread_cond_wait(&cond, &mutex);
  }

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

  CHECK(process != NULL);
  dispatch(process, &ExecutorProcess::sendStatusUpdate, status);

  return 0;
}


int MesosExecutorDriver::sendFrameworkMessage(const string& data)
{
  Lock lock(&mutex);

  if (!running) {
    //executor->error(this, EINVAL, "Executor has exited");
    return -1;
  }

  CHECK(process != NULL);
  dispatch(process, &ExecutorProcess::sendFrameworkMessage, data);

  return 0;
}
