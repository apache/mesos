#include <unistd.h>

#include <iostream>
#include <list>

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

using std::cerr;
using std::cout;
using std::endl;
using std::list;
using std::string;


void usage(const char* argv0)
{
  // Get a list of available commands.
  const string& PATH = os::getenv("PATH");

  list<string> commands;

  foreach (const string& path, strings::split(PATH, ":")) {
    Try<list<string> > matches = os::glob(path::join(path, "mesos-*"));
    if (matches.isSome()) {
      foreach (const string& match, matches.get()) {
        Try<bool> access = os::access(match, X_OK);
        if (access.isSome() && access.get()) {
          Try<string> basename = os::basename(match);
          if (basename.isSome()) {
            commands.push_back(basename.get().substr(6));
          }
        }
      }
    }
  }

  cerr
    << "Usage: " << os::basename(argv0).get() << " <command> [OPTIONS]"
    << endl
    << endl
    << "Available commands:" << endl
    << "    help" << endl;

  foreach (const string& command, commands) {
    cerr << "    " << command << endl;
  }
}


int main(int argc, char** argv)
{
  // Try and add the absolute dirname of argv[0] to PATH so we can
  // find commands (since our installation directory might not be on
  // the path).
  Try<string> dirname = os::dirname(argv[0]);
  if (dirname.isSome()) {
    Result<string> realpath = os::realpath(dirname.get());
    if (realpath.isSome()) {
      os::setenv("PATH", realpath.get() + ":" + os::getenv("PATH"));
    }
  }

  if (argc < 2) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  // Update PYTHONPATH to include path to installed 'mesos' module.
  // TODO(benh): Remove this if/when we install the 'mesos' module via
  // PIP and setuptools.
  string path = path::join(PKGLIBEXECDIR, "python");
  os::setenv("PYTHONPATH", os::getenv("PYTHONPATH", false) + ":" + path);

  // Now dispatch to any mesos-'command' on PATH.
  if (string(argv[1]) == "help") {
    if (argc == 2) {
      usage(argv[0]);
      return EXIT_SUCCESS;
    } else {
      // 'mesos help command' => 'mesos command --help'
      argv[1] = argv[2];
      argv[2] = (char*) "--help";
      return main(argc, argv);
    }
  } else if (string(argv[1]).find("--") == 0) {
    cerr << "Not expecting '" << argv[1] << "' before command" << endl;
    usage(argv[0]);
    return EXIT_FAILURE;
  } else {
    string command = argv[1];
    string executable = "mesos-" + command;
    argv[1] = (char*) executable.c_str();
    execvp(executable.c_str(), argv + 1);
    if (errno == ENOENT) {
      cerr << "'" << command << "' is not a valid command "
           << "(or can not be found)" << endl;
    } else {
      cerr << "Failed to execute " << command
           << ": " << strerror(errno) << endl;
    }
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
