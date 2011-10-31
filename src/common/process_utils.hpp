#ifndef __PROCESS_UTILS_HPP__
#define __PROCESS_UTILS_HPP__

#include <iostream>
#include <sstream>

#include "common/utils.hpp"


namespace mesos {
namespace internal {
namespace utils {
namespace process {

inline Try<int> killtree(
    pid_t pid,
    int signal,
    bool killgroups,
    bool killsess)
{
  if (utils::os::hasenv("MESOS_HOME")) {
    return Try<int>::error("Expecting MESOS_HOME to be set");
  }

  std::string cmdline = utils::os::getenv("MESOS_HOME");
  cmdline += "/killtree.sh";
  cmdline += " -p " + pid;
  cmdline += " -s " + signal;
  if (killgroups) cmdline += " -g";
  if (killsess) cmdline += " -x";

  return utils::os::shell(NULL, cmdline);
}

} // namespace mesos {
} // namespace internal {
} // namespace utils {
} // namespace process {

#endif // __PROCESS_UTILS_HPP__
