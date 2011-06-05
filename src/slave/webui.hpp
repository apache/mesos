#ifndef __SLAVE_WEBUI_HPP__
#define __SLAVE_WEBUI_HPP__

#include <process.hpp>

#include "slave.hpp"

#include "config/config.hpp"


#ifdef MESOS_WEBUI

namespace mesos { namespace internal { namespace slave {

void startSlaveWebUI(const process::PID<Slave>& slave,
                     const Configuration& conf);

}}} // namespace mesos { namespace internal { namespace slave {

#endif // MESOS_WEBUI

#endif // __SLAVE_WEBUI_HPP__
