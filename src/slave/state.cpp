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
    LOG(ERROR) << "Error finding frameworks for slave " << frameworks.error();
    return state;
  }

  foreach (const string& path, frameworks.get()) {
    FrameworkID frameworkId;
    frameworkId.set_value(os::basename(path));

    // Find the executors.
    Try<list<string> > executors =
        os::glob(strings::format(paths::EXECUTOR_PATH, rootDir, slaveId,
                                 frameworkId, "*").get());

    if (executors.isError()) {
      LOG(ERROR) << "Error finding executors for framework "
                 << executors.error();
      continue;
    }

    foreach (const string& path, executors.get()) {
      ExecutorID executorId;
      executorId.set_value(os::basename(path));

      // Find the runs.
      Try<list<string> > runs =
          os::glob(strings::format(paths::EXECUTOR_RUN_PATH, rootDir, slaveId,
                                   frameworkId, executorId, "*").get());

      if (runs.isError()) {
        LOG(ERROR) << "Error finding runs for executor " << runs.error();
        continue;
      }

      foreach (const string& path, runs.get()) {
        Try<int> result = numify<int>(os::basename(path));
        if (!result.isSome()) {
          LOG(ERROR) << "Non-numeric run number in path " << path;
          continue;
        }

        int run = result.get();

        // Update max run.
        state.frameworks[frameworkId].executors[executorId].latest =
            max(run,
                state.frameworks[frameworkId].executors[executorId].latest);

        // Find the tasks.
        Try<list<string> > tasks =
            os::glob(strings::format(paths::TASK_PATH, rootDir, slaveId,
                                     frameworkId, executorId, stringify(run),
                                     "*").get());

        if (tasks.isError()) {
          LOG(WARNING) << "Error finding tasks " << tasks.error();
          continue;
        }

        foreach (const string& path, tasks.get()) {
          TaskID taskId;
          taskId.set_value(os::basename(path));

          state.frameworks[frameworkId].executors[executorId].runs[run].tasks
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

  Try<Nothing> created = os::mkdir(os::dirname(path));

  CHECK(created.isSome())
    << "Error creating directory '" << os::dirname(path)
    << "': " << created.error();

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

  Try<Nothing> created = os::mkdir(os::dirname(path));

  CHECK(created.isSome())
    << "Error creating directory '" << os::dirname(path)
    << "': " << created.error();

  LOG(INFO) << "Writing slave id " << slaveId << " to " << path;

  Try<Nothing> result = os::write(path, stringify(slaveId));

  CHECK(result.isSome())
    << "Error writing slave id to disk " << strerror(errno);
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

  Try<Nothing> created = os::mkdir(os::dirname(path));

  CHECK(created.isSome())
    << "Error creating directory '" << os::dirname(path)
    << "': " << created.error();

  LOG(INFO) << "Writing framework pid " << pid << " to " << path;

  Try<Nothing> result = os::write(path, pid);

  CHECK(result.isSome())
    << "Error writing framework pid to disk " << strerror(errno);
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
