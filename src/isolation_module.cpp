#include "isolation_module.hpp"
#include "process_based_isolation_module.hpp"
#ifdef __sun__
#include "solaris_project_isolation_module.hpp"
#elif __linux__
#include "lxc_isolation_module.hpp"
#endif

using std::string;

using namespace nexus::internal::slave;


IsolationModule * IsolationModule::create(const string &type)
{
  if (type == "process")
    return new ProcessBasedIsolationModule();
#ifdef __sun__
  else if (type == "project")
    return new SolarisProjectIsolationModule();
#elif __linux__
  else if (type == "lxc")
    return new LxcIsolationModule();
#endif

  return NULL;
}


void IsolationModule::destroy(IsolationModule *module)
{
  if (module != NULL)
    delete module;
}
