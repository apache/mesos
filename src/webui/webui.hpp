#ifndef __WEBUI_HPP__
#define __WEBUI_HPP__

#ifdef MESOS_WEBUI

#include <string>
#include <vector>

#include "configurator/configuration.hpp"

namespace mesos {
namespace internal {
namespace webui {

// Starts the Python based webui using the webui directory found via
// any configuration options and the specified webui script (relative
// path from webui directory), and args (accessible in the webui
// script via 'sys.argv' offset by +1).
void start(const Configuration& conf,
           const std::string& script,
           const std::vector<std::string>& args);

} // namespace webui {
} // namespace internal {
} // namespace mesos {

#endif // MESOS_WEBUI

#endif // ___WEBUI_HPP__
