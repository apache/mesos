#ifndef MASTER_WEBUI_HPP
#define MASTER_WEBUI_HPP

#include <process.hpp>

#include "config/config.hpp"

#include "master.hpp"

#ifdef MESOS_WEBUI

namespace mesos { namespace internal { namespace master {

void startMasterWebUI(const PID &master, char* webuiPort);

}}} /* namespace */

#endif /* MESOS_WEBUI */

#endif /* MASTER_WEBUI_HPP */
