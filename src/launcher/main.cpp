#include <mesos.hpp>

#include <boost/lexical_cast.hpp>

#include "launcher.hpp"

using std::string;

using boost::lexical_cast;

using namespace mesos;
using namespace mesos::internal::launcher;


const char * getenvOrFail(const char *variable)
{
  const char *value = getenv(variable);
  if (!value)
    fatal("environment variable %s not set", variable);
  return value;
}


int main(int argc, char **argv)
{
  FrameworkID frameworkId;
  frameworkId.set_value(getenvOrFail("MESOS_FRAMEWORK_ID"));

  ExecutorLauncher(frameworkId,
                   getenvOrFail("MESOS_EXECUTOR_URI"),
                   getenvOrFail("MESOS_USER"),
                   getenvOrFail("MESOS_WORK_DIRECTORY"),
                   getenvOrFail("MESOS_SLAVE_PID"),
                   getenvOrFail("MESOS_FRAMEWORKS_HOME"),
                   getenvOrFail("MESOS_HOME"),
                   getenvOrFail("MESOS_HADOOP_HOME"),
                   lexical_cast<bool>(getenvOrFail("MESOS_REDIRECT_IO")),
                   lexical_cast<bool>(getenvOrFail("MESOS_SWITCH_USER")),
                   map<string, string>()).run();

  return 0;
}
