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

#ifndef __STOUT_INTERNAL_WINDOWS_INHERIT_HPP__
#define __STOUT_INTERNAL_WINDOWS_INHERIT_HPP__

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>

#include <processthreadsapi.h>

namespace internal {
namespace windows {

// This function creates `LPPROC_THREAD_ATTRIBUTE_LIST`, which is used
// to whitelist handles sent to a child process.
typedef _PROC_THREAD_ATTRIBUTE_LIST AttributeList;
inline Result<std::shared_ptr<AttributeList>>
create_attributes_list_for_handles(const std::vector<HANDLE>& handles)
{
  if (handles.empty()) {
    return None();
  }

  SIZE_T size = 0;
  BOOL result = ::InitializeProcThreadAttributeList(
      nullptr, // Pointer to _PROC_THREAD_ATTRIBUTE_LIST. This
               // parameter can be `nullptr` to determine the buffer
               // size required to support the specified number of
               // attributes.
      1,       // Count of attributes to be added to the list.
      0,       // Reserved and must be zero.
      &size);  // Size in bytes required for the attribute list.

  if (result == FALSE) {
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return WindowsError();
    }
  }

  std::shared_ptr<AttributeList> attribute_list(
      reinterpret_cast<AttributeList*>(std::malloc(size)),
      [](AttributeList* p) {
        // NOTE: This delete API does not return anything, nor can it
        // fail, so it is safe to call it regardless of the
        // initialization state of `p`.
        ::DeleteProcThreadAttributeList(p);
        std::free(p);
      });

  if (attribute_list == nullptr) {
    // `std::malloc` failed.
    return WindowsError(ERROR_OUTOFMEMORY);
  }

  result =
    ::InitializeProcThreadAttributeList(attribute_list.get(), 1, 0, &size);

  if (result == FALSE) {
    return WindowsError();
  }

  result = ::UpdateProcThreadAttribute(
      attribute_list.get(),               // Pointer to list structure.
      0,                                  // Reserved and must be 0.
      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,  // The attribute key to update in the
                                          // attribute list.
      const_cast<PVOID*>(handles.data()), // Pointer to the attribute value.
      handles.size() * sizeof(HANDLE),    // Size of the attribute value.
      nullptr,                            // Reserved and must be `nullptr`.
      nullptr);                           // Reserved and must be `nullptr`.

  if (result == FALSE) {
    return WindowsError();
  }

  return attribute_list;
}

// This function enables or disables inheritance for a Windows file handle.
//
// NOTE: By default, handles on Windows are not inheritable, so this is
// primarily used to enable inheritance when passing handles to child processes,
// and subsequently disable inheritance.
inline Try<Nothing> set_inherit(const int_fd& fd, const bool inherit)
{
  const BOOL result = ::SetHandleInformation(
      fd, HANDLE_FLAG_INHERIT, inherit ? HANDLE_FLAG_INHERIT : 0);

  if (result == FALSE) {
    return WindowsError();
  }

  return Nothing();
}

} // namespace windows {
} // namespace internal {

#endif // __STOUT_INTERNAL_WINDOWS_INHERIT_HPP__
