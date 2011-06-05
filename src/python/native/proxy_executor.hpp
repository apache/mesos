#ifndef PROXY_EXECUTOR_HPP
#define PROXY_EXECUTOR_HPP

#ifdef __APPLE__
// Since Python.h defines _XOPEN_SOURCE on Mac OS X, we undefine it
// here so that we don't get warning messages during the build.
#undef _XOPEN_SOURCE
#endif // __APPLE__
#include <Python.h>

#include <string>
#include <vector>

#include <mesos/executor.hpp>


namespace mesos { namespace python {

struct MesosExecutorDriverImpl;

/**
 * Proxy Executor implementation that will call into Python
 */
class ProxyExecutor : public Executor
{
  MesosExecutorDriverImpl *impl;

public:
  ProxyExecutor(MesosExecutorDriverImpl *_impl) : impl(_impl) {}

  virtual ~ProxyExecutor() {}

  virtual void init(ExecutorDriver* driver, const ExecutorArgs& args);

  virtual void launchTask(ExecutorDriver* driver,
                          const TaskDescription& task);

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId);

  virtual void frameworkMessage(ExecutorDriver* driver,
                                const std::string& data);

  virtual void shutdown(ExecutorDriver* driver);

  virtual void error(ExecutorDriver* driver,
                     int code,
                     const std::string& message);
};

}} /* namespace mesos { namespace python { */

#endif /* PROXY_EXECUTOR_HPP */
