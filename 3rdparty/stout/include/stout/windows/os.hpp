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

#ifndef __STOUT_WINDOWS_OS_HPP__
#define __STOUT_WINDOWS_OS_HPP__

#include <direct.h>
#include <io.h>
#include <TlHelp32.h>
#include <Psapi.h>

#include <sys/utime.h>

#include <list>
#include <map>
#include <set>
#include <string>

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/os.hpp>
#include <stout/os/process.hpp>
#include <stout/os/read.hpp>

#include <stout/os/raw/environment.hpp>

namespace os {
namespace internal {

inline Try<OSVERSIONINFOEX> os_version()
{
  OSVERSIONINFOEX os_version;
  os_version.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
  if (!::GetVersionEx(reinterpret_cast<LPOSVERSIONINFO>(&os_version))) {
    return WindowsError(
        "os::internal::os_version: Call to `GetVersionEx` failed");
  }

  return os_version;
}


inline Try<std::string> nodename()
{
  // Get DNS name of the local computer. First, find the size of the output
  // buffer.
  DWORD size = 0;
  if (!::GetComputerNameEx(ComputerNameDnsHostname, nullptr, &size) &&
      ::GetLastError() != ERROR_MORE_DATA) {
    return WindowsError(
        "os::internal::nodename: Call to `GetComputerNameEx` failed");
  }

  std::unique_ptr<char[]> name(new char[size + 1]);

  if (!::GetComputerNameEx(ComputerNameDnsHostname, name.get(), &size)) {
    return WindowsError(
        "os::internal::nodename: Call to `GetComputerNameEx` failed");
  }

  return std::string(name.get());
}


inline std::string machine()
{
  SYSTEM_INFO system_info;
  ::GetNativeSystemInfo(&system_info);

  switch (system_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      return "AMD64";
    case PROCESSOR_ARCHITECTURE_ARM:
      return "ARM";
    case PROCESSOR_ARCHITECTURE_IA64:
      return "IA64";
    case PROCESSOR_ARCHITECTURE_INTEL:
      return "x86";
    default:
      return "Unknown";
  }
}


inline std::string sysname(OSVERSIONINFOEX os_version)
{
  switch (os_version.wProductType) {
    case VER_NT_DOMAIN_CONTROLLER:
    case VER_NT_SERVER:
      return "Windows Server";
    default:
      return "Windows";
  }
}


inline std::string release(OSVERSIONINFOEX os_version)
{
  return stringify(
      Version(os_version.dwMajorVersion, os_version.dwMinorVersion, 0));
}


inline std::string version(OSVERSIONINFOEX os_version)
{
  std::string version = std::to_string(os_version.dwBuildNumber);

  if (os_version.szCSDVersion[0] != '\0') {
    version.append(" ");
    version.append(os_version.szCSDVersion);
  }

  return version;
}

} // namespace internal {


// Overload of os::pids for filtering by groups and sessions. A group / session
// id of 0 will fitler on the group / session ID of the calling process.
// NOTE: Windows does not have the concept of a process group, so we need to
// enumerate all processes.
inline Try<std::set<pid_t>> pids(Option<pid_t> group, Option<pid_t> session)
{
  DWORD max_items = 4096;
  DWORD bytes_returned;
  std::vector<pid_t> processes;
  size_t size_in_bytes;

  // Attempt to populate `processes` with PIDs. We repeatedly call
  // `EnumProcesses` with increasingly large arrays until it "succeeds" at
  // populating the array with PIDs. The criteria for determining when
  // `EnumProcesses` has succeeded are:
  //   (1) the return value is nonzero.
  //   (2) the `bytes_returned` is less than the number of bytes in the array.
  do {
    // TODO(alexnaparu): Set a limit to the memory that can be used.
    processes.resize(max_items);
    size_in_bytes = processes.size() * sizeof(pid_t);
    CHECK_LE(size_in_bytes, MAXDWORD);

    BOOL result = ::EnumProcesses(
        processes.data(),
        static_cast<DWORD>(size_in_bytes),
        &bytes_returned);

    if (!result) {
      return WindowsError("os::pids: Call to `EnumProcesses` failed");
    }

    max_items *= 2;
  } while (bytes_returned >= size_in_bytes);

  return std::set<pid_t>(processes.begin(), processes.end());
}


inline Try<std::set<pid_t>> pids()
{
  return pids(None(), None());
}


// Sets the value associated with the specified key in the set of
// environment variables.
inline void setenv(
    const std::string& key,
    const std::string& value,
    bool overwrite = true)
{
  // Do not set the variable if already set and `overwrite` was not specified.
  //
  // Per MSDN[1], `GetEnvironmentVariable` returns 0 on error and sets the
  // error code to `ERROR_ENVVAR_NOT_FOUND` if the variable was not found.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms683188(v=vs.85).aspx
  if (!overwrite &&
      ::GetEnvironmentVariable(key.c_str(), nullptr, 0) != 0 &&
      ::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
    return;
  }

  // `SetEnvironmentVariable` returns an error code, but we can't act on it.
  ::SetEnvironmentVariable(key.c_str(), value.c_str());
}


// Unsets the value associated with the specified key in the set of
// environment variables.
inline void unsetenv(const std::string& key)
{
  // Per MSDN documentation[1], passing `nullptr` as the value will cause
  // `SetEnvironmentVariable` to delete the key from the process's environment.
  ::SetEnvironmentVariable(key.c_str(), nullptr);
}


// Suspends execution of the calling process until a child specified by `pid`
// has changed state. Unlike the POSIX standard function `::waitpid`, this
// function does not use -1 and 0 to signify errors and nonblocking return.
// Instead, we return `Result<pid_t>`:
//   * In case of error, we return `Error` rather than -1. For example, we
//     would return an `Error` in case of `EINVAL`.
//   * In case of nonblocking return, we return `None` rather than 0. For
//     example, if we pass `WNOHANG` in the `options`, we would expect 0 to be
//     returned in the case that children specified by `pid` exist, but have
//     not changed state yet. In this case we return `None` instead.
//
// NOTE: There are important differences between the POSIX and Windows
// implementations of this function:
//   * On POSIX, `pid_t` is a signed number, but on Windows, PIDs are `DWORD`,
//     which is `unsigned long`. Thus, if we use `DWORD` to represent the `pid`
//     argument, passing -1 as the `pid` would (on most modern servers)
//     silently convert to a really large `pid`. This is undesirable.
//   * Since it is important to be able to detect -1 has been passed to
//     `os::waitpid`, as a matter of practicality, we choose to:
//     (1) Use `long` to represent the `pid` argument.
//     (2) Disable using any value <= 0 for `pid` on Windows.
//   * This decision is pragmatic. The reasoning is:
//     (1) The Windows code paths call `os::waitpid` in only a handful of
//         places, and in none of these conditions do we need `-1` as a value.
//     (2) Since PIDs virtually never take on values outside the range of
//         vanilla signed `long` it is likely that an accidental conversion
//         will never happen.
//     (3) Even though it is not formalized in the C specification, the
//         implementation of `long` on the vast majority of production servers
//         is 2's complement, so we expect that when we accidentally do
//         implicitly convert from `unsigned long` to `long`, we will "wrap
//         around" to negative values. And since we've disabled the negative
//         `pid` in the Windows implementation, we should error out.
//   * Finally, on Windows, we currently do not check that the process we are
//     attempting to await is a child process.
inline Result<pid_t> waitpid(long pid, int* status, int options)
{
  const bool wait_for_child = (options & WNOHANG) == 0;

  // NOTE: Windows does not implement pids <= 0.
  if (pid <= 0) {
    errno = ENOSYS;
    return ErrnoError(
        "os::waitpid: Value of pid is '" + stringify(pid) +
        "'; the Windows implementation currently does not allow values <= 0");
  } else if (options != 0 && options != WNOHANG) {
    // NOTE: We only support `options == 0` or `options == WNOHANG`. On Windows
    // no flags other than `WNOHANG` are supported.
    errno = ENOSYS;
    return ErrnoError(
        "os::waitpid: Only flag `WNOHANG` is implemented on Windows");
  }

  // TODO(hausdorff): Check that `pid` is one of the child processes. If not,
  // set `errno` to `ECHILD` and return -1.

  // Open the child process as a safe `SharedHandle`.
  const HANDLE process = ::OpenProcess(
      PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
      FALSE,
      static_cast<DWORD>(pid));

  if (process == nullptr) {
    return WindowsError("os::waitpid: Failed to open process for pid '" +
                        stringify(pid) + "'");
  }

  SharedHandle scoped_process(process, ::CloseHandle);

  // If `WNOHANG` flag is set, don't wait. Otherwise, wait for child to
  // terminate.
  const DWORD wait_time = wait_for_child ? INFINITE : 0;
  const DWORD wait_results = ::WaitForSingleObject(
      scoped_process.get(),
      wait_time);

  // Verify our wait exited correctly.
  const bool state_signaled = wait_results == WAIT_OBJECT_0;
  if (options == 0 && !state_signaled) {
    // If `WNOHANG` is not set, then we should have stopped waiting only for a
    // state change in `scoped_process`.
    errno = ECHILD;
    return WindowsError(
        "os::waitpid: Failed to wait for pid '" + stringify(pid) +
        "'. `::WaitForSingleObject` should have waited for child process to " +
        "exit, but returned code '" + stringify(wait_results) +
        "' instead");
  } else if (wait_for_child && !state_signaled &&
             wait_results != WAIT_TIMEOUT) {
    // If `WNOHANG` is set, then a successful wait should report either a
    // timeout (since we set the time to wait to `0`), or a successful state
    // change of `scoped_process`. Anything else is an error.
    errno = ECHILD;
    return WindowsError(
        "os::waitpid: Failed to wait for pid '" + stringify(pid) +
        "'. `ENOHANG` flag was passed in, so `::WaitForSingleObject` should " +
        "have either returned `WAIT_OBJECT_0` or `WAIT_TIMEOUT` (the " +
        "timeout was set to 0, because we are not waiting for the child), " +
        "but instead returned code '" + stringify(wait_results) + "'");
  }

  if (!wait_for_child && wait_results == WAIT_TIMEOUT) {
    // Success. `ENOHANG` was set and we got a timeout, so return `None` (POSIX
    // `::waitpid` would return 0 here).
    return None();
  }

  // Attempt to retrieve exit code from child process. Store that exit code in
  // the `status` variable if it's `nullptr`.
  DWORD child_exit_code = 0;
  if (!::GetExitCodeProcess(scoped_process.get(), &child_exit_code)) {
    errno = ECHILD;
    return WindowsError(
        "os::waitpid: Successfully waited on child process with pid '" +
        std::to_string(pid) + "', but could not retrieve exit code");
  }

  if (status != nullptr) {
    *status = child_exit_code;
  }

  // Success. Return pid of the child process for which the status is reported.
  return pid;
}


inline std::string hstrerror(int err)
{
  char buffer[1024];
  DWORD format_error = 0;

  // NOTE: Per the Linux documentation[1], `h_errno` can have only one of the
  // following errors.
  switch (err) {
    case WSAHOST_NOT_FOUND:
    case WSANO_DATA:
    case WSANO_RECOVERY:
    case WSATRY_AGAIN: {
      format_error = ::FormatMessage(
          FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
          nullptr,
          err,
          MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
          buffer,
          sizeof(buffer),
          nullptr);
      break;
    }
    default: {
      return "Unknown resolver error";
    }
  }

  if (format_error == 0) {
    // If call to `FormatMessage` fails, then we choose to output the error
    // code rather than call `FormatMessage` again.
    return "os::hstrerror: Call to `FormatMessage` failed with error code" +
      std::to_string(GetLastError());
  } else {
    return buffer;
  }
}


inline Try<Nothing> chown(
    uid_t uid,
    gid_t gid,
    const std::string& path,
    bool recursive) = delete;


inline Try<Nothing> chmod(const std::string& path, int mode) = delete;


inline Try<Nothing> mknod(
    const std::string& path,
    mode_t mode,
    dev_t dev) = delete;


// Suspends execution for the given duration.
// NOTE: This implementation features a millisecond-resolution sleep API, while
// the POSIX version uses a nanosecond-resolution sleep API. As of this writing,
// Mesos only requires millisecond resolution, so this is ok for now.
inline Try<Nothing> sleep(const Duration& duration)
{
  ::Sleep(static_cast<DWORD>(duration.ms()));

  return Nothing();
}


// Returns the list of files that match the given (shell) pattern.
// NOTE: Deleted on Windows, as a POSIX-API-compliant `glob` is much more
// trouble than its worth, considering our relatively simple usage.
inline Try<std::list<std::string>> glob(const std::string& pattern) = delete;


// Returns the total number of cpus (cores).
inline Try<long> cpus()
{
  SYSTEM_INFO sysInfo;
  ::GetSystemInfo(&sysInfo);
  return static_cast<long>(sysInfo.dwNumberOfProcessors);
}

// Returns load struct with average system loads for the last
// 1, 5 and 15 minutes respectively.
// Load values should be interpreted as usual average loads from
// uptime(1).
inline Try<Load> loadavg()
{
  // No Windows equivalent, return an error until there is a need. We can
  // construct an approximation of this function by periodically polling
  // `GetSystemTimes` and using a sliding window of statistics.
  return WindowsErrorBase(ERROR_NOT_SUPPORTED,
                          "Failed to determine system load averages");
}


// Returns the total size of main and free memory.
inline Try<Memory> memory()
{
  Memory memory;

  MEMORYSTATUSEX memory_status;
  memory_status.dwLength = sizeof(MEMORYSTATUSEX);
  if (!::GlobalMemoryStatusEx(&memory_status)) {
    return WindowsError("os::memory: Call to `GlobalMemoryStatusEx` failed");
  }

  memory.total = Bytes(memory_status.ullTotalPhys);
  memory.free = Bytes(memory_status.ullAvailPhys);
  memory.totalSwap = Bytes(memory_status.ullTotalPageFile);
  memory.freeSwap = Bytes(memory_status.ullAvailPageFile);

  return memory;
}


inline Try<Version> release()
{
  OSVERSIONINFOEX os_version;
  os_version.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
  if (!::GetVersionEx(reinterpret_cast<LPOSVERSIONINFO>(&os_version))) {
    return WindowsError("os::release: Call to `GetVersionEx` failed");
  }

  return Version(os_version.dwMajorVersion, os_version.dwMinorVersion, 0);
}


// Return the system information.
inline Try<UTSInfo> uname()
{
  Try<OSVERSIONINFOEX> os_version = internal::os_version();
  if (os_version.isError()) {
    return Error(os_version.error());
  }

  // Add nodename to `UTSInfo` object.
  Try<std::string> nodename = internal::nodename();
  if (nodename.isError()) {
    return Error(nodename.error());
  }

  // Populate `UTSInfo`.
  UTSInfo info;

  info.sysname = internal::sysname(os_version.get());
  info.release = internal::release(os_version.get());
  info.version = internal::version(os_version.get());
  info.nodename = nodename.get();
  info.machine = internal::machine();

  return info;
}


// Looks in the environment variables for the specified key and
// returns a string representation of its value. If no environment
// variable matching key is found, None() is returned.
inline Option<std::string> getenv(const std::string& key)
{
  DWORD buffer_size = ::GetEnvironmentVariable(key.c_str(), nullptr, 0);
  if (buffer_size == 0) {
    return None();
  }

  std::unique_ptr<char[]> environment(new char[buffer_size]);

  DWORD value_size =
    ::GetEnvironmentVariable(key.c_str(), environment.get(), buffer_size);

  if (value_size == 0) {
    // If `value_size == 0` here, that probably means the environment variable
    // was deleted between when we checked and when we allocated the buffer. We
    // report `None` to indicate the environment variable was not found.
    return None();
  }

  return std::string(environment.get());
}


inline tm* gmtime_r(const time_t* timep, tm* result)
{
  return ::gmtime_s(result, timep) == ERROR_SUCCESS ? result : nullptr;
}


inline Result<PROCESSENTRY32> process_entry(pid_t pid)
{
  // Get a snapshot of the processes in the system. NOTE: We should not check
  // whether the handle is `nullptr`, because this API will always return
  // `INVALID_HANDLE_VALUE` on error.
  HANDLE snapshot_handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, pid);
  if (snapshot_handle == INVALID_HANDLE_VALUE) {
    return WindowsError(
        "os::process_entry: Call to `CreateToolhelp32Snapshot` failed");
  }

  SharedHandle safe_snapshot_handle(snapshot_handle, ::CloseHandle);

  // Initialize process entry.
  PROCESSENTRY32 process_entry;
  ZeroMemory(&process_entry, sizeof(PROCESSENTRY32));
  process_entry.dwSize = sizeof(PROCESSENTRY32);

  // Get first process so that we can loop through process entries until we
  // find the one we care about.
  SetLastError(ERROR_SUCCESS);
  BOOL has_next = Process32First(safe_snapshot_handle.get(), &process_entry);
  if (has_next == FALSE) {
    // No first process was found. We should never be here; it is arguable we
    // should return `None`, since we won't find the PID we're looking for, but
    // we elect to return `Error` because something terrible has probably
    // happened.
    if (GetLastError() != ERROR_SUCCESS) {
      return WindowsError("os::process_entry: Call to `Process32First` failed");
    } else {
      return Error("os::process_entry: Call to `Process32First` failed");
    }
  }

  // Loop through processes until we find the one we're looking for.
  while (has_next == TRUE) {
    if (process_entry.th32ProcessID == pid) {
      // Process found.
      return process_entry;
    }

    has_next = Process32Next(safe_snapshot_handle.get(), &process_entry);
    if (has_next == FALSE) {
      DWORD last_error = GetLastError();
      if (last_error != ERROR_NO_MORE_FILES && last_error != ERROR_SUCCESS) {
        return WindowsError(
            "os::process_entry: Call to `Process32Next` failed");
      }
    }
  }

  return None();
}


// Generate a `Process` object for the process associated with `pid`. If
// process is not found, we return `None`; error is reserved for the case where
// something went wrong.
inline Result<Process> process(pid_t pid)
{
  // Find process with pid.
  Result<PROCESSENTRY32> entry = process_entry(pid);

  if (entry.isError()) {
    return WindowsError(entry.error());
  } else if (entry.isNone()) {
    return None();
  }

  HANDLE process_handle = ::OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
      false,
      pid);

  if (process_handle == INVALID_HANDLE_VALUE) {
    return WindowsError("os::process: Call to `OpenProcess` failed");
  }

  SharedHandle safe_process_handle(process_handle, ::CloseHandle);

  // Get Windows Working set size (Resident set size in linux).
  PROCESS_MEMORY_COUNTERS proc_mem_counters;
  BOOL get_process_memory_info = ::GetProcessMemoryInfo(
      safe_process_handle.get_handle(),
      &proc_mem_counters,
      sizeof(proc_mem_counters));

  if (!get_process_memory_info) {
    return WindowsError("os::process: Call to `GetProcessMemoryInfo` failed");
  }

  // Get session Id.
  pid_t session_id;
  BOOL process_id_to_session_id = ::ProcessIdToSessionId(pid, &session_id);

  if (!process_id_to_session_id) {
    return WindowsError("os::process: Call to `ProcessIdToSessionId` failed");
  }

  // Get Process CPU time.
  FILETIME create_filetime, exit_filetime, kernel_filetime, user_filetime;
  BOOL get_process_times = ::GetProcessTimes(
      safe_process_handle.get_handle(),
      &create_filetime,
      &exit_filetime,
      &kernel_filetime,
      &user_filetime);

  if (!get_process_times) {
    return WindowsError("os::process: Call to `GetProcessTimes` failed");
  }

  // Get utime and stime.
  ULARGE_INTEGER lKernelTime, lUserTime; // In 100 nanoseconds.
  lKernelTime.HighPart = kernel_filetime.dwHighDateTime;
  lKernelTime.LowPart = kernel_filetime.dwLowDateTime;
  lUserTime.HighPart = user_filetime.dwHighDateTime;
  lUserTime.LowPart = user_filetime.dwLowDateTime;

  Try<Duration> utime = Nanoseconds(lKernelTime.QuadPart * 100);
  Try<Duration> stime = Nanoseconds(lUserTime.QuadPart * 100);

  return Process(
      pid,
      entry.get().th32ParentProcessID,         // Parent process id.
      0,                                       // Group id.
      session_id,
      Bytes(proc_mem_counters.WorkingSetSize),
      utime.isSome() ? utime.get() : Option<Duration>::none(),
      stime.isSome() ? stime.get() : Option<Duration>::none(),
      entry.get().szExeFile,                   // Executable filename.
      false);                                  // Is not zombie process.
}


inline int random()
{
  return rand();
}


// `create_job` function creates a job object whose name is derived
// from the `pid` and associates the process with the job object.
// Every process started by the `pid` process which is part of the job
// object becomes part of the job object. The job name should match
// the name used in `kill_job`.
inline Try<HANDLE> create_job(pid_t pid)
{
  Try<std::string> alpha_pid = strings::internal::format("MESOS_JOB_%X", pid);
  if (alpha_pid.isError()) {
    return Error(alpha_pid.error());
  }

  HANDLE process_handle = ::OpenProcess(
      PROCESS_SET_QUOTA | PROCESS_TERMINATE,
      false,
      pid);

  if (process_handle == INVALID_HANDLE_VALUE) {
    return WindowsError("os::create_job: Call to `OpenProcess` failed");
  }

  SharedHandle safe_process_handle(process_handle, ::CloseHandle);

  HANDLE job_handle = ::CreateJobObject(nullptr, alpha_pid.get().c_str());

  if (job_handle == nullptr) {
    return WindowsError("os::create_job: Call to `CreateJobObject` failed");
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { { 0 }, 0 };

  jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  // The job object will be terminated when the job handle closes. This allows
  // the job tree to be terminated in case of errors by closing the handle.
  ::SetInformationJobObject(
      job_handle,
      JobObjectExtendedLimitInformation,
      &jeli,
      sizeof(jeli));

  if (::AssignProcessToJobObject(
          job_handle,
          safe_process_handle.get_handle()) == 0) {
    return WindowsError(
        "os::create_job: Call to `AssignProcessToJobObject` failed");
  };

  return job_handle;
}


// `kill_job` function assumes the process identified by `pid`
// is associated with a job object whose name is derived from it.
// Every process started by the `pid` process which is part of the job
// object becomes part of the job object. Destroying the task
// will close all such processes.
inline Try<Nothing> kill_job(pid_t pid)
{
  Try<std::string> alpha_pid = strings::internal::format("MESOS_JOB_%X", pid);
  if (alpha_pid.isError()) {
    return Error(alpha_pid.error());
  }

  HANDLE job_handle = ::OpenJobObject(
      JOB_OBJECT_TERMINATE,
      FALSE,
      alpha_pid.get().c_str());

  if (job_handle == nullptr) {
    return WindowsError("os::kill_job: Call to `OpenJobObject` failed");
  }

  SharedHandle safe_job_handle(job_handle, ::CloseHandle);

  BOOL result = ::TerminateJobObject(safe_job_handle.get_handle(), 1);
  if (result == 0) {
    return WindowsError();
  }

  return Nothing();
}


inline std::string temp()
{
  // Get temp folder for current user.
  char temp_folder[MAX_PATH + 2];
  if (::GetTempPath(MAX_PATH + 2, temp_folder) == 0) {
    // Failed, try current folder.
    if (::GetCurrentDirectory(MAX_PATH + 2, temp_folder) == 0) {
      // Failed, use relative path.
      return ".";
    }
  }

  return std::string(temp_folder);
}


// Create pipes for interprocess communication. Since the pipes cannot
// be used directly by Posix `read/write' functions they are wrapped
// in file descriptors, a process-local concept.
inline Try<Nothing> pipe(int pipe[2])
{
  // Create inheritable pipe, as described in MSDN[1].
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365782(v=vs.85).aspx
  SECURITY_ATTRIBUTES securityAttr;
  securityAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  securityAttr.bInheritHandle = TRUE;
  securityAttr.lpSecurityDescriptor = nullptr;

  HANDLE read_handle;
  HANDLE write_handle;

  const BOOL result = ::CreatePipe(
      &read_handle,
      &write_handle,
      &securityAttr,
      0);

  pipe[0] = _open_osfhandle(
      reinterpret_cast<intptr_t>(read_handle),
      _O_RDONLY | _O_TEXT);

  if (pipe[0] == -1) {
    return ErrnoError();
  }

  pipe[1] = _open_osfhandle(reinterpret_cast<intptr_t>(write_handle), _O_TEXT);
  if (pipe[1] == -1) {
    return ErrnoError();
  }

  return Nothing();
}


// Prepare the file descriptors to be shared with a different process.
// Under Windows we have to obtain the underlying handles to be shared
// with a different processs.
inline intptr_t fd_to_handle(int in)
{
  return ::_get_osfhandle(in);
}


// Convert the global file handle into a file descriptor, which on
// Windows is only valid within the current process.
inline int handle_to_fd(intptr_t in, int flags)
{
  return ::_open_osfhandle(in, flags);
}

} // namespace os {

#endif // __STOUT_WINDOWS_OS_HPP__
