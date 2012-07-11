#ifndef __WEBUI_HPP__
#define __WEBUI_HPP__

#ifdef MESOS_WEBUI

#include <string>
#include <vector>

namespace mesos {
namespace internal {
namespace webui {

// Starts the Python based webui using the specified webui directory
// and the specified webui script (relative path from webui
// directory), and args (accessible in the webui script via 'sys.argv'
// offset by +1).
void start(const std::string& directory,
           const std::string& script,
           const std::vector<std::string>& args);

} // namespace webui {
} // namespace internal {
} // namespace mesos {

#endif // MESOS_WEBUI

#endif // ___WEBUI_HPP__
