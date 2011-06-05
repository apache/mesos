#ifndef SLAVE_WEBUI_HPP
#define SLAVE_WEBUI_HPP

#include <process.hpp>

#include "config.hpp"
#include "slave.hpp"

#ifdef MESOS_WEBUI

namespace mesos { namespace internal { namespace slave {

void startSlaveWebUI(const PID &slave, char* webuiPort);

}}} /* namespace */

#endif /* MESOS_WEBUI */

#endif /* SLAVE_WEBUI_HPP */
