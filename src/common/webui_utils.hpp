#ifndef __COMMON_WEBUI_UTILS_HPP__
#define __COMMON_WEBUI_UTILS_HPP__

#ifdef MESOS_WEBUI

#include <string>
#include <vector>

#include "configurator/configuration.hpp"

namespace mesos {
namespace internal {
namespace utils {
namespace webui {

// Starts the Python based webui using the webui directory found via
// any configuration options and the specified webui script (relative
// path from webui directory), and args (accessible in the webui
// script via 'sys.argv' offset by +1).
void start(const Configuration& conf,
           const std::string& script,
           const std::vector<std::string>& args);

} // namespace webui {
} // namespace utils {
} // namespace internal {
} // namespace mesos {

#endif // MESOS_WEBUI

#endif // __COMMON_WEBUI_UTILS_HPP__
