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

#ifndef __STOUT_WINDOWS_JOBOBJECT_HPP__
#define __STOUT_WINDOWS_JOBOBJECT_HPP__

#include <algorithm>
#include <numeric>
#include <set>
#include <string>

#include <stout/bytes.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/os.hpp>
#include <stout/os/process.hpp>

namespace os {

// `name_job` maps a `pid` to a `wstring` name for a job object.
// Only named job objects are accessible via `OpenJobObject`.
// Thus all our job objects must be named. This is essentially a shim
// to map the Linux concept of a process tree's root `pid` to a
// named job object so that the process group can be treated similarly.
inline Try<std::wstring> name_job(pid_t pid)
{
  Try<std::string> alpha_pid = strings::internal::format("MESOS_JOB_%X", pid);
  if (alpha_pid.isError()) {
    return Error(alpha_pid.error());
  }
  return wide_stringify(alpha_pid.get());
}


// `open_job` returns a safe shared handle to the named job object `name`.
// `desired_access` is a job object access rights flag.
// `inherit_handles` if true, processes created by this
// process will inherit the handle. Otherwise, the processes
// do not inherit this handle.
inline Try<SharedHandle> open_job(
    const DWORD desired_access,
    const BOOL inherit_handles,
    const std::wstring& name)
{
  SharedHandle job_handle(
      ::OpenJobObjectW(desired_access, inherit_handles, name.data()),
      ::CloseHandle);

  if (job_handle.get_handle() == nullptr) {
    return WindowsError(
        "os::open_job: Call to `OpenJobObject` failed for job: " +
        stringify(name));
  }

  return job_handle;
}


inline Try<SharedHandle> open_job(
    const DWORD desired_access, const BOOL inherit_handles, const pid_t pid)
{
  const Try<std::wstring> name = os::name_job(pid);
  if (name.isError()) {
    return Error(name.error());
  }

  return open_job(desired_access, inherit_handles, name.get());
}

// `create_job` function creates a named job object using `name`.
inline Try<SharedHandle> create_job(const std::wstring& name)
{
  SharedHandle job_handle(
      ::CreateJobObjectW(
          nullptr,       // Use a default security descriptor, and
                         // the created handle cannot be inherited.
          name.data()),  // The name of the job.
      ::CloseHandle);

  if (job_handle.get_handle() == nullptr) {
    return WindowsError(
        "os::create_job: Call to `CreateJobObject` failed for job: " +
        stringify(name));
  }

  return job_handle;
}


// `get_job_info` gets the job object information for the process group
// represented by `pid`, assuming it is assigned to a job object. This function
// will fail otherwise.
//
// https://msdn.microsoft.com/en-us/library/windows/desktop/ms684925(v=vs.85).aspx // NOLINT(whitespace/line_length)
inline Try<JOBOBJECT_BASIC_ACCOUNTING_INFORMATION> get_job_info(pid_t pid)
{
  Try<SharedHandle> job_handle = os::open_job(JOB_OBJECT_QUERY, false, pid);
  if (job_handle.isError()) {
    return Error(job_handle.error());
  }

  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = {};

  const BOOL result = ::QueryInformationJobObject(
      job_handle->get_handle(),
      JobObjectBasicAccountingInformation,
      &info,
      sizeof(info),
      nullptr);
  if (result == FALSE) {
    return WindowsError(
        "os::get_job_info: call to `QueryInformationJobObject` failed");
  }

  return info;
}


template <size_t max_pids>
Result<std::set<Process>> _get_job_processes(const SharedHandle& job_handle)
{
  // This is a statically allocated `JOBOBJECT_BASIC_PROCESS_ID_LIST`. We lie to
  // the Windows API and construct our own struct to avoid (a) having to do
  // hairy size calculations and (b) having to allocate dynamically, and then
  // worry about deallocating.
  struct
  {
    DWORD NumberOfAssignedProcesses;
    DWORD NumberOfProcessIdsInList;
    DWORD ProcessIdList[max_pids];
  } pid_list;

  const BOOL result = ::QueryInformationJobObject(
      job_handle.get_handle(),
      JobObjectBasicProcessIdList,
      reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(&pid_list),
      sizeof(pid_list),
      nullptr);

  // `ERROR_MORE_DATA` indicates we need a larger `max_pids`.
  if (result == FALSE && ::GetLastError() == ERROR_MORE_DATA) {
    return None();
  }

  if (result == FALSE) {
    return WindowsError(
        "os::_get_job_processes: call to `QueryInformationJobObject` failed");
  }

  std::set<Process> processes;
  for (DWORD i = 0; i < pid_list.NumberOfProcessIdsInList; ++i) {
    Result<Process> process = os::process(pid_list.ProcessIdList[i]);
    if (process.isSome()) {
      processes.insert(process.get());
    }
  }

  return processes;
}


inline Try<std::set<Process>> get_job_processes(pid_t pid)
{
  // TODO(andschwa): Overload open_job to use pid.
  Try<SharedHandle> job_handle = os::open_job(JOB_OBJECT_QUERY, false, pid);
  if (job_handle.isError()) {
    return Error(job_handle.error());
  }

  // Try to enumerate the processes with three sizes: 32, 1K, and 32K.

  Result<std::set<Process>> result =
    os::_get_job_processes<32>(job_handle.get());
  if (result.isError()) {
    return Error(result.error());
  } else if (result.isSome()) {
    return result.get();
  }

  result = os::_get_job_processes<32 * 32>(job_handle.get());
  if (result.isError()) {
    return Error(result.error());
  } else if (result.isSome()) {
    return result.get();
  }

  result = os::_get_job_processes<32 * 32 * 32>(job_handle.get());
  if (result.isError()) {
    return Error(result.error());
  } else if (result.isSome()) {
    return result.get();
  }

  // If it was bigger than 32K, something else has gone wrong.

  return Error("os::get_job_processes: failed to get processes");
}


inline Try<Bytes> get_job_mem(pid_t pid)
{
  const Try<std::set<Process>> processes = os::get_job_processes(pid);
  if (processes.isError()) {
    return Error(processes.error());
  }

  return std::accumulate(
      processes->cbegin(),
      processes->cend(),
      Bytes(0),
      [](const Bytes& bytes, const Process& process) {
        if (process.rss.isNone()) {
          return bytes;
        }

        return bytes + process.rss.get();
      });
}


// `set_job_kill_on_close_limit` causes the job object to terminate all
// processes assigned to it when the last handle to the job object is closed.
// This can be used to limit the lifetime of the process group represented by
// the job object. Without this limit set, the processes will continue to run.
inline Try<Nothing> set_job_kill_on_close_limit(pid_t pid)
{
  Try<SharedHandle> job_handle =
    os::open_job(JOB_OBJECT_SET_ATTRIBUTES, false, pid);

  if (job_handle.isError()) {
    return Error(job_handle.error());
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  const BOOL result = ::SetInformationJobObject(
      job_handle->get_handle(),
      JobObjectExtendedLimitInformation,
      &info,
      sizeof(info));

  if (result == FALSE) {
    return WindowsError(
        "os::set_job_kill_on_close_limit: call to `SetInformationJobObject` "
        "failed");
  }

  return Nothing();
}


// `set_job_cpu_limit` sets a CPU limit for the process represented by
// `pid`, assuming it is assigned to a job object. This function will fail
// otherwise. This limit is a hard cap enforced by the OS.
//
// https://msdn.microsoft.com/en-us/library/windows/desktop/hh448384(v=vs.85).aspx // NOLINT(whitespace/line_length)
inline Try<Nothing> set_job_cpu_limit(pid_t pid, double cpus)
{
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION control_info = {};
  control_info.ControlFlags =
    JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;

  // This `CpuRate` is the number of cycles per 10,000 cycles, or a percentage
  // times 100, e.g. 20% yields 20 * 100 = 2,000. However, the `cpus` argument
  // represents 1 CPU core with `1.0`, so a 100% CPU limit on a quad-core
  // machine would be `4.0 cpus`. Thus a mapping of `cpus` to `CpuRate` is
  // `(cpus / os::cpus()) * 100 * 100`, or the requested `cpus` divided by the
  // number of CPUs to obtain a fractional representation, multiplied by 100 to
  // make it a percentage, multiplied again by 100 to become a `CpuRate`.
  //
  // Mathematically, we're normalizing the requested CPUS to a range
  // of [1, 10000] cycles. However, because the input is not
  // sanitized, we have to handle the edge case of the ratio being
  // greater than 1. So we take the `min(max(ratio * 10000, 1),
  // 10000)`. We don't consider going out of bounds an error because
  // CPU limitations are inherently imprecise.
  const long total_cpus = os::cpus().get(); // This doesn't fail on Windows.
  // This must be constrained. We don't care about perfect precision.
  const long cycles = static_cast<long>((cpus / total_cpus) * 10000L);
  const long cpu_rate = std::min(std::max(cycles, 1L), 10000L);
  control_info.CpuRate = static_cast<DWORD>(cpu_rate);
  Try<SharedHandle> job_handle =
    os::open_job(JOB_OBJECT_SET_ATTRIBUTES, false, pid);
  if (job_handle.isError()) {
    return Error(job_handle.error());
  }

  const BOOL result = ::SetInformationJobObject(
      job_handle->get_handle(),
      JobObjectCpuRateControlInformation,
      &control_info,
      sizeof(control_info));
  if (result == FALSE) {
    return WindowsError(
        "os::set_job_cpu_limit: call to `SetInformationJobObject` failed");
  }

  return Nothing();
}


// `set_job_mem_limit` sets a memory limit for the process represented by
// `pid`, assuming it is assigned to a job object. This function will fail
// otherwise. This limit is a hard cap enforced by the OS.
//
// https://msdn.microsoft.com/en-us/library/windows/desktop/ms684156(v=vs.85).aspx // NOLINT(whitespace/line_length)
inline Try<Nothing> set_job_mem_limit(pid_t pid, Bytes limit)
{
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_JOB_MEMORY;
  info.JobMemoryLimit = limit.bytes();

  Try<SharedHandle> job_handle =
    os::open_job(JOB_OBJECT_SET_ATTRIBUTES, false, pid);
  if (job_handle.isError()) {
    return Error(job_handle.error());
  }

  const BOOL result = ::SetInformationJobObject(
      job_handle->get_handle(),
      JobObjectExtendedLimitInformation,
      &info,
      sizeof(info));
  if (result == FALSE) {
    return WindowsError(
        "os::set_job_mem_limit: call to `SetInformationJobObject` failed");
  }

  return Nothing();
}


// `assign_job` assigns a process with `pid` to the job object `job_handle`.
// Every process started by the `pid` process using `CreateProcess`
// will also be owned by the job object.
inline Try<Nothing> assign_job(SharedHandle job_handle, pid_t pid)
{
  // Get process handle for `pid`.
  SharedHandle process_handle(
      ::OpenProcess(
          // Required access rights to assign to a Job Object.
          PROCESS_SET_QUOTA | PROCESS_TERMINATE,
          false, // Don't inherit handle.
          pid),
      ::CloseHandle);

  if (process_handle.get_handle() == nullptr) {
    return WindowsError("os::assign_job: Call to `OpenProcess` failed");
  }

  const BOOL result = ::AssignProcessToJobObject(
      job_handle.get_handle(), process_handle.get_handle());

  if (result == FALSE) {
    return WindowsError(
        "os::assign_job: Call to `AssignProcessToJobObject` failed");
  };

  return Nothing();
}


// The `kill_job` function wraps the Windows sytem call `TerminateJobObject`
// for the job object `job_handle`. This will call `TerminateProcess`
// for every associated child process.
inline Try<Nothing> kill_job(SharedHandle job_handle)
{
  const BOOL result = ::TerminateJobObject(
      job_handle.get_handle(),
      // The exit code to be used by all processes in the job object.
      1);

  if (result == FALSE) {
    return WindowsError("os::kill_job: Call to `TerminateJobObject` failed");
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_WINDOWS_JOBOBJECT_HPP__
