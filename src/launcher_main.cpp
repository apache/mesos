#include "launcher.hpp"

#include "nexus.hpp"

#include <boost/lexical_cast.hpp>

using std::string;

using boost::lexical_cast;

using namespace nexus;
using namespace nexus::internal::launcher;


const char * getenvOrFail(const char *variable) {
  const char *value = getenv(variable);
  if (!value)
    fatal("environment variable %s not set", variable);
  return value;
}


int main(int argc, char **argv)
{
  string_map params;   // Empty map
  ExecutorLauncher(lexical_cast<FrameworkID>(getenvOrFail("NEXUS_FRAMEWORK_ID")),
                   getenvOrFail("NEXUS_EXECUTOR_URI"),
                   getenvOrFail("NEXUS_USER"),
                   getenvOrFail("NEXUS_WORK_DIRECTORY"),
                   getenvOrFail("NEXUS_SLAVE_PID"),
                   lexical_cast<bool>(getenvOrFail("NEXUS_REDIRECT_IO")),
                   params).run();
  return 0;
}
