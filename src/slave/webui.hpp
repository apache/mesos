#ifndef SLAVE_WEBUI_HPP
#define SLAVE_WEBUI_HPP

#include <process.hpp>

#include "slave.hpp"

#include "common/params.hpp"

#include "config/config.hpp"

#ifdef MESOS_WEBUI

namespace mesos { namespace internal { namespace slave {

void startSlaveWebUI(const PID &slave, const Params &params);

}}} /* namespace */

#endif /* MESOS_WEBUI */

#endif /* SLAVE_WEBUI_HPP */
