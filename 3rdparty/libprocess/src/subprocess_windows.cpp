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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <string>

#include <glog/logging.h>

#include <process/future.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/os/strerror.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/internal/windows/longpath.hpp>

using std::array;
using std::string;

namespace process {

using InputFileDescriptors = Subprocess::IO::InputFileDescriptors;
using OutputFileDescriptors = Subprocess::IO::OutputFileDescriptors;

namespace internal {

static Try<HANDLE> duplicateHandle(const HANDLE handle)
{
  HANDLE duplicate = INVALID_HANDLE_VALUE;

  // TODO(anaparu): Do we need to scope the duplicated handle
  // to the child process?
  BOOL result = ::DuplicateHandle(
      ::GetCurrentProcess(),  // Source process == current.
      handle,                 // Handle to duplicate.
      ::GetCurrentProcess(),  // Target process == current.
      &duplicate,
      0,                      // Ignored (DUPLICATE_SAME_ACCESS).
      TRUE,                   // Inheritable handle.
      DUPLICATE_SAME_ACCESS); // Same access level as source.

  if (!result) {
    return WindowsError("Failed to duplicate handle of stdin file");
  }

  return duplicate;
}


static Try<HANDLE> getHandleFromFileDescriptor(int_fd fd)
{
  // Extract handle from file descriptor.
  const HANDLE handle = fd;
  if (handle == INVALID_HANDLE_VALUE) {
    return WindowsError("Failed to get `HANDLE` for file descriptor");
  }

  return handle;
}


static Try<HANDLE> getHandleFromFileDescriptor(
    const int_fd fd,
    const Subprocess::IO::FDType type)
{
  Try<HANDLE> handle = getHandleFromFileDescriptor(fd);
  if (handle.isError()) {
    return Error(handle.error());
  }

  switch (type) {
    case Subprocess::IO::DUPLICATED: {
      const Try<HANDLE> duplicate = duplicateHandle(handle.get());

      if (duplicate.isError()) {
        return Error(duplicate.error());
      }

      return duplicate;
    }
    case Subprocess::IO::OWNED:
      return handle;

    // NOTE: By not setting a default we leverage the compiler
    // errors when the enumeration is augmented to find all
    // the cases we need to provide. Same for below.
  }
}


// Creates a file for a subprocess's stdin, stdout, or stderr.
//
// NOTE: For portability, Libprocess implements POSIX-style semantics for these
// files, and make no assumptions about who has access to them. For example, we
// do not enforce a process-level write lock on stdin, and we do not enforce a
// similar read lock from stdout.
//
// TODO(hausdorff): Rethink name here, write a comment about this function.
static Try<HANDLE> createIoPath(const string& path, DWORD accessFlags)
{
  // Per function comment, the flags `FILE_SHARE_READ`, `FILE_SHARE_WRITE`, and
  // `OPEN_ALWAYS` are all important. The former two make sure we do not
  // acquire a process-level lock on reading/writing the file, and the last one
  // ensures that we open the file if it exists, and create it if it does not.
  // Note that we specify both `FILE_SHARE_READ` and `FILE_SHARE_WRITE` because
  // the documentation is not clear about whether `FILE_SHARE_WRITE` also
  // ensures we don't take a read lock out.
  SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
  const HANDLE handle = ::CreateFileW(
      ::internal::windows::longpath(path).data(),
      accessFlags,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &sa,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);

  if (handle == INVALID_HANDLE_VALUE) {
    return WindowsError("Failed to open '" + path + "'");
  }

  return handle;
}


static Try<HANDLE> createInputFile(const string& path)
{
  // Get a handle to the `stdin` file. Use `GENERIC_READ` and
  // `FILE_SHARE_READ` to make the handle read-only (as `stdin` should
  // be), but allow others to read from the same file.
  return createIoPath(path, GENERIC_READ);
}


static Try<HANDLE> createOutputFile(const string& path)
{
  // Get a handle to the `stdout` file. Use `GENERIC_WRITE` to make the
  // handle writeable (as `stdout` should be), but still allow other processes
  // to read from the file.
  return createIoPath(path, GENERIC_WRITE);
}

}  // namespace internal {

// Opens an inheritable pipe[1] represented as a pair of file handles. On
// success, the first handle returned receives the 'read' handle of the pipe,
// while the second receives the 'write' handle. The pipe handles can then be
// passed to a child process, as exemplified in [2].
//
// [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa379560(v=vs.85).aspx
// [2] https://msdn.microsoft.com/en-us/library/windows/desktop/ms682499(v=vs.85).aspx
Subprocess::IO Subprocess::PIPE()
{
  return Subprocess::IO(
      []() -> Try<InputFileDescriptors> {
        const Try<array<os::WindowsFD, 2>> handles = os::pipe();
        if (handles.isError()) {
          return Error(handles.error());
        }

        // Create STDIN pipe and set the 'write' component to not be
        // inheritable.
        if (!::SetHandleInformation(handles.get()[1], HANDLE_FLAG_INHERIT, 0)) {
          return WindowsError(
              "PIPE: Failed to call SetHandleInformation on stdin pipe");
        }

        InputFileDescriptors fds;
        fds.read = handles.get()[0];
        fds.write = handles.get()[1];
        return fds;
      },
      []() -> Try<OutputFileDescriptors> {
        const Try<array<os::WindowsFD, 2>> handles = os::pipe();
        if (handles.isError()) {
          return Error(handles.error());
        }

        // Create OUT pipe and set the 'read' component to not be inheritable.
        if (!::SetHandleInformation(handles.get()[0], HANDLE_FLAG_INHERIT, 0)) {
          return WindowsError(
              "PIPE: Failed to call SetHandleInformation on out pipe");
        }

        OutputFileDescriptors fds;
        fds.read = handles.get()[0];
        fds.write = handles.get()[1];
        return fds;
      });
}


Subprocess::IO Subprocess::PATH(const string& path)
{
  return Subprocess::IO(
      [path]() -> Try<InputFileDescriptors> {
        const Try<HANDLE> inHandle = internal::createInputFile(path);

        if (inHandle.isError()) {
          return Error(inHandle.error());
        }

        InputFileDescriptors inDescriptors;
        inDescriptors.read = inHandle.get();
        return inDescriptors;
      },
      [path]() -> Try<OutputFileDescriptors> {
        const Try<HANDLE> outHandle = internal::createOutputFile(path);

        if (outHandle.isError()) {
          return Error(outHandle.error());
        }

        OutputFileDescriptors outDescriptors;
        outDescriptors.write = outHandle.get();
        return outDescriptors;
      });
}

}  // namespace process {
