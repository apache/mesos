#ifndef MASTER_WEBUI_HPP
#define MASTER_WEBUI_HPP

#include <process.hpp>

#include "master.hpp"

#include "common/params.hpp"

#include "config/config.hpp"


#ifdef MESOS_WEBUI

namespace mesos { namespace internal { namespace master {

void startMasterWebUI(const PID &master, const Params &params);

}}} /* namespace */

#endif /* MESOS_WEBUI */

#endif /* MASTER_WEBUI_HPP */
