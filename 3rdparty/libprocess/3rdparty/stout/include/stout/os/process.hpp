#ifndef __STOUT_OS_PROCESS_HPP__
#define __STOUT_OS_PROCESS_HPP__

#include <sys/types.h> // For pid_t.

#include <string>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>

namespace os {

struct Process
{
  Process(pid_t _pid,
          pid_t _parent,
          pid_t _group,
          pid_t _session,
          const Bytes& _rss,
          const Duration& _utime,
          const Duration& _stime,
          const std::string& _command)
    : pid(_pid),
      parent(_parent),
      group(_group),
      session(_session),
      rss(_rss),
      utime(_utime),
      stime(_stime),
      command(_command) {}

  const pid_t pid;
  const pid_t parent;
  const pid_t group;
  const pid_t session;
  const Bytes rss;
  const Duration utime;
  const Duration stime;
  const std::string command;

  // TODO(bmahler): Add additional data as needed.

  bool operator <  (const Process& p) const { return pid <  p.pid; }
  bool operator <= (const Process& p) const { return pid <= p.pid; }
  bool operator >  (const Process& p) const { return pid >  p.pid; }
  bool operator >= (const Process& p) const { return pid >= p.pid; }
  bool operator == (const Process& p) const { return pid == p.pid; }
  bool operator != (const Process& p) const { return pid != p.pid; }
};

} // namespace os {

#endif // __STOUT_OS_PROCESS_HPP__
