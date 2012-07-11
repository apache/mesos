#ifdef MESOS_WEBUI

#include <Python.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/uio.h>

#include <tr1/functional>

#include "common/strings.hpp"
#include "common/thread.hpp"
#include "common/utils.hpp"

#include "webui/webui.hpp"

namespace mesos {
namespace internal {
namespace webui {

static void run(const std::string& directory,
                const std::string& script,
                const std::vector<std::string>& args)
{
  // Setup the Python interpreter and load the script.
  std::string path = utils::path::join(directory, script);

  Py_Initialize();

  // Setup argv for Python interpreter.
  char** argv = new char*[args.size() + 1];

  argv[0] = const_cast<char*>(path.c_str());

  for (int i = 0; i < args.size(); i++) {
    argv[i + 1] = const_cast<char*>(args[i].c_str());
  }

  PySys_SetArgv(args.size() + 1, argv);

  // Run some code to setup PATH and add webui_dir as a variable.
  std::string code =
    "import sys\n"
    "sys.path.append('" + utils::path::join(directory, "common") + "')\n"
    "sys.path.append('" + utils::path::join(directory, "bottle-0.8.3") + "')\n";

  PyRun_SimpleString(code.c_str());

  LOG(INFO) << "Loading webui script at '" << path << "'";

  FILE* file = fopen(path.c_str(), "r");
  PyRun_SimpleFile(file, path.c_str());
  fclose(file);

  Py_Finalize();

  delete[] argv;
}


void wait(int fd)
{
  char temp[8];
  if (read(fd, temp, 8) == -1) {
    PLOG(FATAL) << "Failed to read on pipe from parent";
  }
  exit(1);
}


void start(const std::string& directory,
           const std::string& script,
           const std::vector<std::string>& args)
{
  // Make sure script is a relative path.
  CHECK(script[0] != '/')
    << "Expecting relative path for webui script (relative to 'webui_dir')";

  // Make sure directory/script exists.
  std::string path = utils::path::join(directory, script);

  CHECK(utils::os::exists(path))
    << "Failed to find webui script at '" << path << "'";

  // TODO(benh): Consider actually forking a process for the webui
  // rather than just creating a thread. This will let us more easily
  // run multiple webui's simultaneously (e.g., the master and
  // slave). This might also give us better isolation from Python
  // interpreter errors (and maybe even remove the need for two C-c to
  // exit the process).

  int pipes[2];
  pipe(pipes);

  pid_t pid;
  if ((pid = fork()) == -1) {
    PLOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid) {
    // In parent process.
    close(pipes[0]); // Close the reader end in the parent.
  } else {
    // In child process.
    close(pipes[1]); // Close the writer end in the child.

    if (!thread::start(std::tr1::bind(&wait, pipes[0]), true)) {
      LOG(FATAL) << "Failed to start wait thread";
    }

    run(directory, script, args);
  }
}

} // namespace webui {
} // namespace internal {
} // namespace mesos {

#endif // MESOS_WEBUI
