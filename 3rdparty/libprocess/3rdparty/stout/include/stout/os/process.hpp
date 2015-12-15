// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_PROCESS_HPP__
#define __STOUT_OS_PROCESS_HPP__

#include <sys/types.h> // For pid_t.

#include <list>
#include <ostream>
#include <sstream>
#include <string>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>


namespace os {

struct Process
{
  Process(pid_t _pid,
          pid_t _parent,
          pid_t _group,
          const Option<pid_t>& _session,
          const Option<Bytes>& _rss,
          const Option<Duration>& _utime,
          const Option<Duration>& _stime,
          const std::string& _command,
          bool _zombie)
    : pid(_pid),
      parent(_parent),
      group(_group),
      session(_session),
      rss(_rss),
      utime(_utime),
      stime(_stime),
      command(_command),
      zombie(_zombie) {}

  const pid_t pid;
  const pid_t parent;
  const pid_t group;
  const Option<pid_t> session;
  const Option<Bytes> rss;
  const Option<Duration> utime;
  const Option<Duration> stime;
  const std::string command;
  const bool zombie;

  // TODO(bmahler): Add additional data as needed.

  bool operator<(const Process& p) const { return pid < p.pid; }
  bool operator<=(const Process& p) const { return pid <= p.pid; }
  bool operator>(const Process& p) const { return pid > p.pid; }
  bool operator>=(const Process& p) const { return pid >= p.pid; }
  bool operator==(const Process& p) const { return pid == p.pid; }
  bool operator!=(const Process& p) const { return pid != p.pid; }
};


class ProcessTree
{
public:
  // Returns a process subtree rooted at the specified PID, or none if
  // the specified pid could not be found in this process tree.
  Option<ProcessTree> find(pid_t pid) const
  {
    if (process.pid == pid) {
      return *this;
    }

    foreach (const ProcessTree& tree, children) {
      Option<ProcessTree> option = tree.find(pid);
      if (option.isSome()) {
        return option;
      }
    }

    return None();
  }

  // Checks if the specified pid is contained in this process tree.
  bool contains(pid_t pid) const
  {
    return find(pid).isSome();
  }

  operator Process() const
  {
    return process;
  }

  operator pid_t() const
  {
    return process.pid;
  }

  const Process process;
  const std::list<ProcessTree> children;

private:
  friend struct Fork;
  friend Try<ProcessTree> pstree(pid_t, const std::list<Process>&);

  ProcessTree(
      const Process& _process,
      const std::list<ProcessTree>& _children)
    : process(_process),
      children(_children) {}
};


inline std::ostream& operator<<(std::ostream& stream, const ProcessTree& tree)
{
  if (tree.children.empty()) {
    stream << "--- " << tree.process.pid << " ";
    if (tree.process.zombie) {
      stream << "(" << tree.process.command << ")";
    } else {
      stream << tree.process.command;
    }
  } else {
    stream << "-+- " << tree.process.pid << " ";
    if (tree.process.zombie) {
      stream << "(" << tree.process.command << ")";
    } else {
      stream << tree.process.command;
    }
    size_t size = tree.children.size();
    foreach (const ProcessTree& child, tree.children) {
      std::ostringstream out;
      out << child;
      stream << "\n";
      if (--size != 0) {
        stream << " |" << strings::replace(out.str(), "\n", "\n |");
      } else {
        stream << " \\" << strings::replace(out.str(), "\n", "\n  ");
      }
    }
  }
  return stream;
}

} // namespace os {


// An overload of stringify for printing a list of process trees
// (since printing a process tree is rather particular).
inline std::string stringify(const std::list<os::ProcessTree>& list)
{
  std::ostringstream out;
  out << "[ " << std::endl;
  std::list<os::ProcessTree>::const_iterator iterator = list.begin();
  while (iterator != list.end()) {
    out << stringify(*iterator);
    if (++iterator != list.end()) {
      out << std::endl << std::endl;
    }
  }
  out << std::endl << "]";
  return out.str();
}

#endif // __STOUT_OS_PROCESS_HPP__
