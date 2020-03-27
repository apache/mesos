// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_WINDOWS_SUBPROCESS_HPP__
#define __PROCESS_WINDOWS_SUBPROCESS_HPP__

#include <signal.h>

#include <array>
#include <string>

#include <glog/logging.h>

#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/close.hpp>
#include <stout/os/environment.hpp>
#include <stout/os/exec.hpp>

#include <userEnv.h>

namespace process {
namespace internal {

// NOTE: We are expecting that components of `argv` that need to be quoted
// (for example, paths with spaces in them like `C:\"Program Files"\foo.exe`)
// to have been already quoted correctly before we generate `command`.
// Incorrectly-quoted command arguments will probably lead the child process
// to terminate with an error. See also NOTE on `process::subprocess`.
inline Try<os::windows::internal::ProcessData> createChildProcess(
    const std::string& path,
    const std::vector<std::string>& argv,
    const Option<std::map<std::string, std::string>>& environment,
    const std::vector<Subprocess::ParentHook>& parent_hooks,
    const InputFileDescriptors& stdinfds,
    const OutputFileDescriptors& stdoutfds,
    const OutputFileDescriptors& stderrfds,
    const std::vector<int_fd>& whitelist_fds = {})
{
  const std::array<int_fd, 3> fds{
    stdinfds.read, stdoutfds.write, stderrfds.write};

  Try<os::windows::internal::ProcessData> process_data =
    os::windows::internal::create_process(
        path,
        argv,
        environment,
        true, // Create suspended.
        fds,
        whitelist_fds);

  // Close the child-ends of the file descriptors that are created
  // by this function.
  foreach (const int_fd& fd, fds) {
    if (fd.is_valid()) {
      Try<Nothing> result = os::close(fd);
      if (result.isError()) {
        return Error(result.error());
      }
    }
  }

  if (process_data.isError()) {
    return Error(process_data.error());
  }

  // Run the parent hooks.
  const pid_t pid = process_data->pid;
  foreach (const Subprocess::ParentHook& hook, parent_hooks) {
    Try<Nothing> parent_setup = hook.parent_setup(pid);

    // If the hook callback fails, we shouldn't proceed with the
    // execution and hence the child process should be killed.
    if (parent_setup.isError()) {
      // Attempt to kill the process. Since it is still in suspended state, we
      // do not need to kill any descendents. We also can't use `os::kill_job`
      // because this process is not in a Job Object unless one of the parent
      // hooks added it.
      ::TerminateProcess(process_data->process_handle.get_handle(), 1);

      return Error(
          "Failed to execute Parent Hook in child '" + stringify(pid) +
          "' with command '" + stringify(argv) + "': " +
          parent_setup.error());
    }
  }

  // Start child process.
  if (::ResumeThread(process_data->thread_handle.get_handle()) == -1) {
    return WindowsError(
        "Failed to resume child process with command '" +
        stringify(argv) + "'");
  }

  return process_data;
}

}  // namespace internal {
}  // namespace process {

#endif // __PROCESS_WINDOWS_SUBPROCESS_HPP__
