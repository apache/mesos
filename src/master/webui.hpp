#ifndef __MASTER_WEBUI_HPP__
#define __MASTER_WEBUI_HPP__

#include <process.hpp>

#include "master.hpp"

#include "config/config.hpp"


#ifdef MESOS_WEBUI

namespace mesos { namespace internal { namespace master {

void startMasterWebUI(const PID& master, const Configuration& conf);

}}} /* namespace */

#endif /* MESOS_WEBUI */

#endif /* __MASTER_WEBUI_HPP__ */
