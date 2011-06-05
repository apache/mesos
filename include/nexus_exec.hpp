#ifndef NEXUS_EXEC_HPP
#define NEXUS_EXEC_HPP

#include <string>

#include <nexus.hpp>


namespace nexus {

class ExecutorDriver;

namespace internal { class ExecutorProcess; }


/**
 * Arguments passed to executors on initialization.
 */
struct ExecutorArgs
{
  ExecutorArgs() {}

  ExecutorArgs(SlaveID _slaveId, const std::string& _host,
      FrameworkID _frameworkId, const std::string& _frameworkName,
      const data_string& _data)
    : slaveId(_slaveId), host(_host), frameworkId(_frameworkId),
      frameworkName(_frameworkName), data(_data) {};

  SlaveID slaveId;
  std::string host;
  FrameworkID frameworkId;
  std::string frameworkName;
  data_string data;
};


/**
 * Callback interface to be implemented by frameworks' executors.
 */
class Executor
{
public:
  virtual ~Executor() {}

  virtual void init(ExecutorDriver* d, const ExecutorArgs& args) {}
  virtual void launchTask(ExecutorDriver* d, const TaskDescription& task) {}
  virtual void killTask(ExecutorDriver* d, TaskID taskId) {}
  virtual void frameworkMessage(ExecutorDriver* d,
                                const FrameworkMessage& message) {}
  virtual void shutdown(ExecutorDriver* d) {}
  virtual void error(ExecutorDriver* d, int code, const std::string& message);
};


/**
 * Abstract interface for driving an executor connected to Nexus.
 * This interface is used both to start the executor running (and
 * communicating with the slave) and to send information from the executor
 * to Nexus (such as status updates). Concrete implementations of
 * ExecutorDriver will take a Executor as a parameter in order to make
 * callbacks into it on various events.
 */
class ExecutorDriver
{
public:
  virtual ~ExecutorDriver() {}

  // Lifecycle methods
  virtual int start() { return -1; }
  virtual int stop() { return -1; }
  virtual int join() { return -1; }
  virtual int run() { return -1; } // Start and then join driver

  // Communication methods from executor to Nexus
  virtual int sendStatusUpdate(const TaskStatus& status) { return -1; }
  virtual int sendFrameworkMessage(const FrameworkMessage& message) { return -1; }
};


/**
 * Concrete implementation of ExecutorDriver that communicates with a
 * Nexus slave. The slave's location is read from environment variables
 * set by it when it execs the user's executor script; users only need
 * to create the NexusExecutorDriver and call run() on it.
 */
class NexusExecutorDriver : public ExecutorDriver
{
public:
  NexusExecutorDriver(Executor* executor);
  virtual ~NexusExecutorDriver();

  // Lifecycle methods
  virtual int start();
  virtual int stop();
  virtual int join();
  virtual int run(); // Start and then join driver

  virtual int sendStatusUpdate(const TaskStatus& status);
  virtual int sendFrameworkMessage(const FrameworkMessage& message);

  // Executor getter; required by some of the SWIG proxies
  virtual Executor* getExecutor() { return executor; }

private:
  friend class internal::ExecutorProcess;

  Executor* executor;

  // LibProcess process for communicating with slave
  internal::ExecutorProcess* process;

  // Are we currently registered with the slave
  bool running;
  
  // Mutex to enforce all non-callbacks are execute serially
  pthread_mutex_t mutex;

  // Condition variable for waiting until driver terminates
  pthread_cond_t cond;
};

} /* namespace nexus { */

#endif /* NEXUS_EXEC_HPP */
