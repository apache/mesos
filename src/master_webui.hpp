#ifndef MASTER_WEBUI_HPP
#define MASTER_WEBUI_HPP

#include <process.hpp>

#include "config.hpp"
#include "master.hpp"

#ifdef NEXUS_WEBUI

namespace nexus { namespace internal { namespace master {

void startMasterWebUI(const PID &master, char* webuiPort);

}}} /* namespace */

#endif /* NEXUS_WEBUI */

#endif /* MASTER_WEBUI_HPP */
