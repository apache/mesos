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

#ifndef __PROCESS_WINDOWS_JOBOBJECT_HPP__
#define __PROCESS_WINDOWS_JOBOBJECT_HPP__

#include <map>
#include <string>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/windows.hpp> // For `SharedHandle`.

#include <stout/os/windows/jobobject.hpp>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>
#include <process/process.hpp>

#include <glog/logging.h> // For `CHECK` macro.


namespace process {
namespace internal {

class JobObjectManager : public Process<JobObjectManager>
{
public:
  JobObjectManager() : ProcessBase("__job_object_manager") {}
  virtual ~JobObjectManager() {}

  void manage(
      const pid_t pid,
      const std::wstring& name,
      const SharedHandle& handle)
  {
    jobs.emplace(pid, JobData{name, handle});

    process::reap(pid)
      .onAny(defer(self(), &Self::cleanup, lambda::_1, pid));
  }

protected:
  void cleanup(Future<Option<int>> exit_code, const pid_t pid)
  {
    CHECK(!exit_code.isPending());
    CHECK(!exit_code.isDiscarded());

    Try<Nothing> killJobResult = os::kill_job(jobs.at(pid).handle);
    CHECK(!killJobResult.isError())
      << "Failed to kill job object: " << killJobResult.error();

    // Finally, erase the `JobData`, closing the last handle to the job object.
    // All functionality requiring a live job object handle (but possibly a
    // dead process) must happen prior to this, e.g. in a another parent hook.
    jobs.erase(pid);
  }

private:
  struct JobData {
    std::wstring name;
    SharedHandle handle;
  };

  std::map<pid_t, JobData> jobs;
};

// Global job object manager process. Defined in `process.cpp`.
extern PID<JobObjectManager> job_object_manager;

} // namespace internal {

inline Subprocess::ParentHook Subprocess::ParentHook::CREATE_JOB() {
  return Subprocess::ParentHook([](pid_t pid) -> Try<Nothing> {
    // NOTE: There are two very important parts to this hook. First, Windows
    // does not have a process hierarchy in the same sense that Unix does, so
    // in order to be able to kill a task, we have to put it in a job object.
    // Then, when we terminate the job object, it will terminate all the
    // processes in the task (including any processes that were subsequently
    // created by any process in this task). Second, the lifetime of the job
    // object is greater than the lifetime of the processes it contains. Thus
    // the job object handle is explicitly owned by the global job object
    // manager process.
    Try<std::wstring> name = os::name_job(pid);
    if (name.isError()) {
      return Error(name.error());
    }

    // This creates a named job object in the Windows kernel.
    // This handle must remain in scope (and open) until
    // a running process is assigned to it.
    Try<SharedHandle> handle = os::create_job(name.get());
    if (handle.isError()) {
      return Error(handle.error());
    }

    // This actually assigns the process `pid` to the job object.
    Try<Nothing> result = os::assign_job(handle.get(), pid);
    if (result.isError()) {
      return Error(result.error());
    }

    // Save the handle to the job object to ensure the object remains
    // open for the entire lifetime of the agent process, and is closed
    // when the process is reaped.
    dispatch(
      process::internal::job_object_manager,
      &process::internal::JobObjectManager::manage,
      pid,
      name.get(),
      handle.get());

    return Nothing();
  });
}

} // namespace process {

#endif // __PROCESS_WINDOWS_JOBOBJECT_HPP__
