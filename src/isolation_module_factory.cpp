#include "isolation_module_factory.hpp"
#include "process_based_isolation_module.hpp"
#ifdef __sun__
#include "solaris_project_isolation_module.hpp"
#elif __linux__
#include "lxc_isolation_module.hpp"
#endif

using namespace mesos::internal::slave;


DEFINE_FACTORY(IsolationModule, Slave *)
{
  registerClass<ProcessBasedIsolationModule>("process");
#ifdef __sun__
  registerClass<SolarisProjectIsolationModule>("project");
#elif __linux__
  registerClass<LxcIsolationModule>("lxc");
#endif
}
