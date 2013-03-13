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


// Helper to checkpoint string to disk, with necessary error checking.
void checkpoint(const std::string& path, const std::string& message)
{
  // Create the base directory.
  CHECK_SOME(os::mkdir(os::dirname(path).get()))
    << "Failed to create directory '" << os::dirname(path).get() << "'";

  // Now checkpoint the message to disk.
  CHECK_SOME(os::write(path, message))
    << "Failed to checkpoint " << message << " to '" << path << "'";
}


// Helper to checkpoint protobuf to disk, with necessary error checking.
void checkpoint(
    const std::string& path,
    const google::protobuf::Message& message)
{
  // Create the base directory.
  CHECK_SOME(os::mkdir(os::dirname(path).get()))
    << "Failed to create directory '" << os::dirname(path).get() << "'";

  // Now checkpoint the protobuf to disk.
  CHECK_SOME(protobuf::write(path, message))
    << "Failed to checkpoint " << message.DebugString()
    << " to '" << path << "'";
}

} // namespace state {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
