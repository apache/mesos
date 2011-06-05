#ifndef __MASTER_WEBUI_HPP__
#define __MASTER_WEBUI_HPP__

#include <process.hpp>

#include "master.hpp"

#include "config/config.hpp"


#ifdef MESOS_WEBUI

namespace mesos { namespace internal { namespace master {

void startMasterWebUI(Master* master, const Configuration& conf);

}}} // namespace mesos { namespace internal { namespace master {

#endif // MESOS_WEBUI

#endif // __MASTER_WEBUI_HPP__ 
