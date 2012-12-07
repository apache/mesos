#include <glog/logging.h>

#include "stout/foreach.hpp"
#include "stout/format.hpp"
#include "stout/numify.hpp"
#include "stout/os.hpp"
#include "stout/protobuf.hpp"
#include "stout/try.hpp"

#include "slave/paths.hpp"
#include "slave/state.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace state {

using std::list;
using std::string;
using std::max;

SlaveState parse(const string& rootDir, const SlaveID& slaveId)
{
  SlaveState state;

  const string& slaveDir = paths::getSlavePath(rootDir, slaveId);
  state.slaveMetaDir = slaveDir;

  // Find the frameworks.
  Try<list<string> > frameworks = os::glob(
      strings::format(paths::FRAMEWORK_PATH, rootDir, slaveId, "*").get());

  if (frameworks.isError()) {
    LOG(ERROR) << "Failed to find frameworks for slave: " << frameworks.error();
    return state;
  }

  foreach (const string& path, frameworks.get()) {
    FrameworkID frameworkId;
    frameworkId.set_value(os::basename(path).get());

    // Find the executors.
    Try<list<string> > executors =
        os::glob(strings::format(paths::EXECUTOR_PATH, rootDir, slaveId,
                                 frameworkId, "*").get());

    if (executors.isError()) {
      LOG(ERROR) << "Failed to find executors for framework: "
                 << executors.error();
      continue;
    }

    foreach (const string& path, executors.get()) {
      ExecutorID executorId;
      executorId.set_value(os::basename(path).get());

      // Find the runs.
      Try<list<string> > runs =
          os::glob(strings::format(paths::EXECUTOR_RUN_PATH, rootDir, slaveId,
                                   frameworkId, executorId, "*").get());

      if (runs.isError()) {
        LOG(ERROR) << "Failed to find runs for executor: " << runs.error();
        continue;
      }

      foreach (const string& path, runs.get()) {
        if (os::basename(path).get() == paths::EXECUTOR_LATEST_SYMLINK) {
          // TODO(vinod): Store the latest UUID in the state.
          continue;
        }

        const UUID& uuid = UUID::fromString(os::basename(path).get());

        // Find the tasks.
        Try<list<string> > tasks =
            os::glob(strings::format(paths::TASK_PATH, rootDir, slaveId,
                                     frameworkId, executorId, uuid.toString(),
                                     "*").get());

        if (tasks.isError()) {
          LOG(WARNING) << "Failed to find tasks: " << tasks.error();
          continue;
        }

        foreach (const string& path, tasks.get()) {
          TaskID taskId;
          taskId.set_value(os::basename(path).get());

          state.frameworks[frameworkId].executors[executorId].runs[uuid].tasks
            .insert(taskId);
        }
      }
    }
  }
  return state;
}


// Helper functions for check-pointing slave data.

void writeTask(Task* task, const string& taskDir)
{
  const string& path = taskDir + "/task";

  Try<Nothing> created = os::mkdir(taskDir);

  CHECK_SOME(created) << "Failed to create task directory '" << taskDir << "'";

  LOG(INFO) << "Writing task description for task "
            << task->task_id() << " to " << path;

  Try<bool> result = protobuf::write(path, *task);

  if (result.isError()) {
    LOG(FATAL) << "Failed to write task description to disk " << result.error();
  }
}


void writeSlaveID(const string& rootDir, const SlaveID& slaveId)
{
  const string& path = paths::getSlaveIDPath(rootDir);

  Try<Nothing> created = os::mkdir(os::dirname(path).get());

  CHECK_SOME(created)
    << "Failed to create directory '" << os::dirname(path).get() << "'";

  LOG(INFO) << "Writing slave id " << slaveId << " to " << path;

  Try<Nothing> result = os::write(path, stringify(slaveId));

  CHECK_SOME(result) << "Failed to write slave id to disk";
}


SlaveID readSlaveID(const string& rootDir)
{
  const string& path = paths::getSlaveIDPath(rootDir);

  Result<string> result = os::read(path);

  SlaveID slaveId;

  if (!result.isSome()) {
    LOG(WARNING) << "Cannot read slave id from " << path << " because "
                 << result.isError() ? result.error() : "empty";
    return slaveId;
  }

  LOG(INFO) << "Read slave id " << result.get() << " from " << path;

  slaveId.set_value(result.get());
  return slaveId;
}


void writeFrameworkPID(const string& metaRootDir,
                       const SlaveID& slaveId,
                       const FrameworkID& frameworkId,
                       const string& pid)
{
  const string& path = paths::getFrameworkPIDPath(metaRootDir, slaveId,
                                                  frameworkId);

  Try<Nothing> created = os::mkdir(os::dirname(path).get());

  CHECK_SOME(created)
    << "Failed to create directory '" << os::dirname(path).get() << "'";

  LOG(INFO) << "Writing framework pid " << pid << " to " << path;

  Try<Nothing> result = os::write(path, pid);

  CHECK_SOME(result) << "Failed to write framework pid to disk";
}


process::UPID readFrameworkPID(const string& metaRootDir,
                               const SlaveID& slaveId,
                               const FrameworkID& frameworkId)
{
  const string& path = paths::getFrameworkPIDPath(metaRootDir, slaveId,
                                                  frameworkId);

  Result<string> result = os::read(path);

  if (!result.isSome()) {
    LOG(WARNING) << "Cannot read framework pid from " << path << " because "
                 << result.isError() ? result.error() : "empty";
    return process::UPID();
  }

  LOG(INFO) << "Read framework pid " << result.get() << " from " << path;

  return process::UPID(result.get());
}

} // namespace state {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
