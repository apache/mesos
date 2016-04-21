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
  if (!::GetComputerNameEx(ComputerNameDnsHostname, NULL, &size) &&
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
    BOOL result = ::EnumProcesses(processes.data(), size_in_bytes,
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
      ::GetEnvironmentVariable(key.c_str(), NULL, 0) != 0 &&
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
  // Per MSDN documentation[1], passing `NULL` as the value will cause
  // `SetEnvironmentVariable` to delete the key from the process's environment.
  ::SetEnvironmentVariable(key.c_str(), NULL);
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
          NULL,
          err,
          MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
          buffer,
          sizeof(buffer),
          NULL);
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


// This function is a portable version of execvpe ('p' means searching
// executable from PATH and 'e' means setting environments). We add
// this function because it is not available on all systems.
inline int execvpe(const char* file, char** argv, char** envp) = delete;


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
  DWORD buffer_size = ::GetEnvironmentVariable(key.c_str(), NULL, 0);
  if (buffer_size == 0) {
    return None();
  }

  std::unique_ptr<char> environment(new char[buffer_size]);

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
  return ::gmtime_s(result, timep) == ERROR_SUCCESS ? result : NULL;
}


inline Try<bool> access(const std::string& fileName, int how)
{
  if (::_access(fileName.c_str(), how) != 0) {
    return ErrnoError("os::access: Call to `_access` failed for path '" +
                      fileName + "'");
  }

  return true;
}

inline Result<PROCESSENTRY32> process_entry(pid_t pid)
{
  // Get a snapshot of the processes in the system. NOTE: We should not check
  // whether the handle is `NULL`, because this API will always return
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
  bool has_next = Process32First(safe_snapshot_handle.get(), &process_entry);
  if (!has_next) {
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
  while (has_next) {
    if (process_entry.th32ProcessID == pid) {
      // Process found.
      return process_entry;
    }

    has_next = Process32Next(safe_snapshot_handle.get(), &process_entry);
    if (!has_next) {
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
      safe_process_handle.get(),
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
      safe_process_handle.get(),
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


} // namespace os {

#endif // __STOUT_WINDOWS_OS_HPP__
