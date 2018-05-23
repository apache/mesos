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

#ifndef __STOUT_INTERNAL_WINDOWS_OVERLAPPED_HPP__
#define __STOUT_INTERNAL_WINDOWS_OVERLAPPED_HPP__

#include <climits>
#include <type_traits>

#include <stout/result.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>

namespace internal {

namespace windows {

// Helper function that creates an overlapped object that can be used
// safely for synchronous IO.
inline Try<OVERLAPPED> init_overlapped_for_sync_io()
{
  OVERLAPPED overlapped = {};

  // Creating the event is a defensive measure in the case where multiple
  // simultaneous overlapped operations are happening on the same file.
  // If there is no event, then any IO completion on the file can signal
  // the overlapped object, instead of just the requested IO event.
  // For more details, see
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms686358(v=vs.85).aspx // NOLINT(whitespace/line_length)
  //
  // The parameters to `::CreateEventW` will create a non-inheritable,
  // auto-resetting, non-signaled, unamed event.
  overlapped.hEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (overlapped.hEvent == nullptr) {
    return WindowsError();
  }

  // According to the MSDN docs, "a valid event handle whose low-order bit
  // is set keeps I/O completion from being queued to the completion port" [1].
  // This is another defensive measure to prevent memory corruption if this
  // function is called when the fd is associated with a completion port.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa364986(v=vs.85).aspx // NOLINT(whitespace/line_length)
  overlapped.hEvent = reinterpret_cast<HANDLE>(
      reinterpret_cast<uintptr_t>(overlapped.hEvent) | 1);

  return overlapped;
}


// Windows uses a combination of the return code and the Win32 error code to
// determine that status of the overlapped IO functions (success, failure,
// pending). This function wraps that logic into a `Result` type so it's
// easier to process.
//
// This function returns:
//   - `Some`:  The number of bytes read/written if the async function had
//              finished synchronously.
//   - `Error`: The error code of the failed asynchronous function.
//   - `None`:  None if the asynchronous function was scheduled and is pending.
inline Result<size_t> process_async_io_result(
    bool successful_return_code, size_t bytes_transfered)
{
  // IO is already complete, so the result is already in `bytes_transfered`.
  if (successful_return_code) {
    return bytes_transfered;
  }

  const WindowsError error;
  if (error.code == ERROR_IO_PENDING) {
    // IO is pending.
    return None();
  }

  // Other errors are fatal errors.
  return error;
}

} // namespace windows {

} // namespace internal {

#endif // __STOUT_INTERNAL_WINDOWS_OVERLAPPED_HPP__
