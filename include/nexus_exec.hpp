#ifndef NEXUS_EXEC_HPP
#define NEXUS_EXEC_HPP

#include <string>

#include <nexus.hpp>

namespace nexus {

namespace internal { class ExecutorProcess; }

struct ExecutorArgs
{
  ExecutorArgs() {}

  ExecutorArgs(SlaveID _slaveId, FrameworkID _frameworkId,
      const std::string& _frameworkName, const data_string& _data)
    : slaveId(_slaveId), frameworkId(_frameworkId),
      frameworkName(_frameworkName), data(_data) {};

  SlaveID slaveId;
  FrameworkID frameworkId;
  std::string frameworkName;
  data_string data;
};


class Executor
{
public:
  Executor();
  virtual ~Executor();

  // Overridable callbacks
  virtual void init(const ExecutorArgs& args) {}
  virtual void startTask(const TaskDescription& task) {}
  virtual void killTask(TaskID taskId) {}
  virtual void frameworkMessage(const FrameworkMessage& message) {}
  virtual void shutdown();
  virtual void error(int code, const std::string& message);

  // Non-overridable lifecycle methods
  void run();

  // Non-overridable communication methods
  void sendStatusUpdate(const TaskStatus &status);
  void sendFrameworkMessage(const FrameworkMessage &message);

private:
  friend class internal::ExecutorProcess;
  
  // Mutex to enforce all non-callbacks are execute serially
  pthread_mutex_t mutex;
};

} /* namespace nexus { */

#endif /* NEXUS_EXEC_HPP */
