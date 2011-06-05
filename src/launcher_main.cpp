#include "launcher.hpp"

#include "mesos.hpp"

#include <boost/lexical_cast.hpp>

using std::string;

using boost::lexical_cast;

using namespace mesos;
using namespace mesos::internal::launcher;


const char * getenvOrFail(const char *variable) {
  const char *value = getenv(variable);
  if (!value)
    fatal("environment variable %s not set", variable);
  return value;
}


int main(int argc, char **argv)
{
  string_map params;   // Empty map
  ExecutorLauncher(lexical_cast<FrameworkID>(getenvOrFail("MESOS_FRAMEWORK_ID")),
                   getenvOrFail("MESOS_EXECUTOR_URI"),
                   getenvOrFail("MESOS_USER"),
                   getenvOrFail("MESOS_WORK_DIRECTORY"),
                   getenvOrFail("MESOS_SLAVE_PID"),
                   lexical_cast<bool>(getenvOrFail("MESOS_REDIRECT_IO")),
                   params).run();
  return 0;
}
