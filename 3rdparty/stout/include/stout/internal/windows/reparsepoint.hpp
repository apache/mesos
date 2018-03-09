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

#ifndef __STOUT_INTERNAL_WINDOWS_REPARSEPOINT_HPP__
#define __STOUT_INTERNAL_WINDOWS_REPARSEPOINT_HPP__

#include <string>

#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/mkdir.hpp>

#include <stout/internal/windows/attributes.hpp>
#include <stout/internal/windows/longpath.hpp>


namespace os {
namespace stat {

// Specify whether symlink path arguments should be followed or
// not. APIs in the os::stat family that take a FollowSymlink
// argument all provide FollowSymlink::FOLLOW_SYMLINK as the default value,
// so they will follow symlinks unless otherwise specified.
enum class FollowSymlink
{
  DO_NOT_FOLLOW_SYMLINK,
  FOLLOW_SYMLINK
};

} // namespace stat {
} // namespace os {


namespace internal {
namespace windows {

// We pass this struct to `DeviceIoControl` to get information about a reparse
// point (including things like whether it's a symlink). It is normally part of
// the Device Driver Kit (DDK), specifically `nitfs.h`, but rather than taking
// a dependency on the DDK, we choose to simply copy the struct here. This is a
// well-worn path used by Boost FS[1], among others. See documentation
// here[2][3].
//
// [1] http://www.boost.org/doc/libs/1_46_1/libs/filesystem/v3/src/operations.cpp // NOLINT(whitespace/line_length)
// [2] https://msdn.microsoft.com/en-us/library/cc232007.aspx
// [3] https://msdn.microsoft.com/en-us/library/cc232005.aspx
typedef struct _REPARSE_DATA_BUFFER
{
  // Describes, among other things, which type of reparse point this is (e.g.,
  // a symlink).
  ULONG ReparseTag;
  // Size in bytes of common portion of the `REPARSE_DATA_BUFFER`.
  USHORT ReparseDataLength;
  // Unused. Ignore.
  USHORT Reserved;
  // Holds symlink data.
  struct
  {
    // Byte offset in `PathBuffer` where the substitute name begins.
    // Calculated as an offset from 0.
    USHORT SubstituteNameOffset;
    // Length in bytes of the substitute name.
    USHORT SubstituteNameLength;
    // Byte offset in `PathBuffer` where the print name begins. Calculated as
    // an offset from 0.
    USHORT PrintNameOffset;
    // Length in bytes of the print name.
    USHORT PrintNameLength;
    // Indicates whether symlink is absolute or relative. If flags containing
    // `SYMLINK_FLAG_RELATIVE`, then the substitute name is a relative
    // symlink.
    ULONG Flags;
    // The path string; the Windows declaration is the first byte, but we
    // declare a suitably sized array so we can use this struct more easily. The
    // "path string" itself is a Unicode `wchar` array containing both
    // substitute name and print name. They can occur in any order. Use the
    // offset and length of each in this struct to calculate where each starts
    // and ends.
    //
    // https://msdn.microsoft.com/en-us/library/windows/hardware/ff552012(v=vs.85).aspx // NOLINT(whitespace/line_length)
    WCHAR PathBuffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
  } SymbolicLinkReparseBuffer;
} REPARSE_DATA_BUFFER;


// Convenience struct for holding symlink data, meant purely for internal use.
// We pass this around instead of the `REPARSE_DATA_BUFFER` struct, simply
// because this struct is easier to deal with and reason about.
struct SymbolicLink
{
  std::wstring substitute_name;
  std::wstring print_name;
  ULONG flags;
};


// Checks file/folder attributes for a path to see if the reparse point
// attribute is set; this indicates whether the path points at a reparse point,
// rather than a "normal" file or folder.
inline Try<bool> reparse_point_attribute_set(const std::wstring& absolute_path)
{
  const Try<DWORD> attributes = get_file_attributes(absolute_path.data());
  if (attributes.isError()) {
    return Error(attributes.error());
  }

  return (attributes.get() & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}


// Attempts to extract symlink data out of a `REPARSE_DATA_BUFFER` (which could
// hold other things, e.g., mount point data).
inline Try<SymbolicLink> build_symbolic_link(const REPARSE_DATA_BUFFER& data)
{
  const bool is_symLink = (data.ReparseTag & IO_REPARSE_TAG_SYMLINK) != 0;
  if (!is_symLink) {
    return Error("Data buffer is not a symlink");
  }

  // NOTE: This buffer is not null terminated.
  const WCHAR* substitute_name =
    data.SymbolicLinkReparseBuffer.PathBuffer +
    data.SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(WCHAR);
  const size_t substitute_name_length =
    data.SymbolicLinkReparseBuffer.SubstituteNameLength / sizeof(WCHAR);

  // NOTE: This buffer is not null terminated.
  const WCHAR* print_name =
    data.SymbolicLinkReparseBuffer.PathBuffer +
    data.SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(WCHAR);
  const size_t print_name_length =
    data.SymbolicLinkReparseBuffer.PrintNameLength / sizeof(WCHAR);

  return SymbolicLink{
      std::wstring(substitute_name, substitute_name_length),
      std::wstring(print_name, print_name_length),
      data.SymbolicLinkReparseBuffer.Flags};
}


// Attempts to get a file or folder handle for an absolute path, and follows
// symlinks. That is, if the path points at a symlink, the handle will refer to
// the file or folder the symlink points at, rather than the symlink itself.
inline Try<SharedHandle> get_handle_follow(const std::string& absolute_path)
{
  const Try<DWORD> attributes = get_file_attributes(longpath(absolute_path));

  if (attributes.isError()) {
    return Error(attributes.error());
  }

  bool resolved_path_is_directory = attributes.get() & FILE_ATTRIBUTE_DIRECTORY;

  // NOTE: The name of `CreateFile` is misleading: it is also used to retrieve
  // handles to existing files or directories as if it were actually `OpenPath`
  // (which does not exist). We use `OPEN_EXISTING` but not
  // `FILE_FLAG_OPEN_REPARSE_POINT` to explicitily follow (resolve) symlinks in
  // the path to the file or directory.
  //
  // Note also that `CreateFile` will appropriately generate a handle for
  // either a folder or a file, as long as the appropriate flag is being set:
  // `FILE_FLAG_BACKUP_SEMANTICS`.
  //
  // The `FILE_FLAG_BACKUP_SEMANTICS` flag is being set whenever the target is
  // a directory. According to MSDN[1]: "You must set this flag to obtain a
  // handle to a directory. A directory handle can be passed to some functions
  // instead of a file handle". More `FILE_FLAG_BACKUP_SEMANTICS` documentation
  // can be found in MSDN[2].
  //
  // The `GENERIC_READ` flag is being used because it's the most common way of
  // opening a file for reading only. The `SHARE` flags allow other processes
  // to read the file at the same time, as well as allow this process to read
  // files that were also opened with these flags. MSDN[1] provides a more
  // detailed explanation of these flags.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx // NOLINT(whitespace/line_length)
  // [2] https://msdn.microsoft.com/en-us/library/windows/desktop/aa364399(v=vs.85).aspx // NOLINT(whitespace/line_length)
  const DWORD access_flags = resolved_path_is_directory
    ? FILE_FLAG_BACKUP_SEMANTICS
    : FILE_ATTRIBUTE_NORMAL;

  const HANDLE handle = ::CreateFileW(
      longpath(absolute_path).data(),
      GENERIC_READ,     // Open the file for reading only.
      // Must pass in all SHARE flags below, in case file is already open.
      // Otherwise, we may get an access denied error.
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,          // Ignored.
      OPEN_EXISTING,    // Open existing file.
      access_flags,     // Open file, not the symlink itself.
      nullptr);         // Ignored.

  if (handle == INVALID_HANDLE_VALUE) {
    return WindowsError();
  }

  return SharedHandle(handle, ::CloseHandle);
}


// Attempts to get a file or folder handle for an absolute path, and does not
// follow symlinks. That is, if the path points at a symlink, the handle will
// refer to the symlink rather than the file or folder the symlink points at.
inline Try<SharedHandle> get_handle_no_follow(const std::string& absolute_path)
{
  const Try<DWORD> attributes = get_file_attributes(longpath(absolute_path));

  if (attributes.isError()) {
    return Error(attributes.error());
  }

  bool resolved_path_is_directory = attributes.get() & FILE_ATTRIBUTE_DIRECTORY;

  // NOTE: According to the `CreateFile` documentation[1], the `OPEN_EXISTING`
  // and `FILE_FLAG_OPEN_REPARSE_POINT` flags need to be used when getting a
  // handle for the symlink.
  //
  // Note also that `CreateFile` will appropriately generate a handle for
  // either a folder or a file, as long as the appropriate flag is being set:
  // `FILE_FLAG_BACKUP_SEMANTICS` or `FILE_FLAG_OPEN_REPARSE_POINT`.
  //
  // The `FILE_FLAG_BACKUP_SEMANTICS` flag is being set whenever the target is
  // a directory. According to MSDN[1]: "You must set this flag to obtain a
  // handle to a directory. A directory handle can be passed to some functions
  // instead of a file handle". More `FILE_FLAG_BACKUP_SEMANTICS` documentation
  // can be found in MSDN[2].
  //
  // The `GENERIC_READ` flag is being used because it's the most common way of
  // opening a file for reading only. The `SHARE` flags allow other processes
  // to read the file at the same time, as well as allow this process to read
  // files that were also opened with these flags. MSDN[1] provides a more
  // detailed explanation of these flags.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx // NOLINT(whitespace/line_length)
  // [2] https://msdn.microsoft.com/en-us/library/windows/desktop/aa364399(v=vs.85).aspx // NOLINT(whitespace/line_length)
  const DWORD access_flags = resolved_path_is_directory
    ? (FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS)
    : FILE_FLAG_OPEN_REPARSE_POINT;

  const HANDLE handle = ::CreateFileW(
      longpath(absolute_path).data(),
      GENERIC_READ,     // Open the file for reading only.
      // Must pass in all SHARE flags below, in case file is already open.
      // Otherwise, we may get an access denied error.
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,          // Ignored.
      OPEN_EXISTING,    // Open existing symlink.
      access_flags,     // Open symlink, not the file it points to.
      nullptr);         // Ignored.

  if (handle == INVALID_HANDLE_VALUE) {
    return WindowsError();
  }

  return SharedHandle(handle, ::CloseHandle);
}


// Attempts to get the symlink data for a file or folder handle.
inline Try<SymbolicLink> get_symbolic_link_data(const HANDLE handle)
{
  // To get the symlink data, we call `DeviceIoControl`. This function is part
  // of the Device Driver Kit (DDK)[1] and, along with `FSCTL_GET_REPARSE_POINT`
  // is used to emit information about reparse points (and, thus, symlinks,
  // since symlinks are implemented with reparse points). This technique is
  // being used in Boost FS code as well[2].
  //
  // Summarized, the documentation tells us that we need to pass in
  // `FSCTL_GET_REPARSE_POINT` to get the function to populate a
  // `REPARSE_DATA_BUFFER` struct with data about a reparse point.
  // The `REPARSE_DATA_BUFFER` struct is defined in a DDK header file,
  // so to avoid bringing in a multitude of DDK headers we take a cue from
  // Boost FS, and copy the struct into this header (see above).
  //
  // Finally, for context, it may be worth looking at the MSDN
  // documentation[3] for `DeviceIoControl` itself.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa364571(v=vs.85).aspx // NOLINT(whitespace/line_length)
  // [2] https://svn.boost.org/trac/boost/ticket/4663
  // [3] https://msdn.microsoft.com/en-us/library/windows/desktop/aa363216(v=vs.85).aspx // NOLINT(whitespace/line_length)
  REPARSE_DATA_BUFFER buffer;
  DWORD ignored = 0;

  // The semantics of this function are: get the reparse data associated with
  // the `handle` of some open directory or file, and that data in
  // `reparse_point_data`.
  const BOOL reparse_data_obtained = ::DeviceIoControl(
      handle,                   // Handle to file or directory.
      FSCTL_GET_REPARSE_POINT,  // Gets reparse point data for file/folder.
      nullptr,                  // Ignored.
      0,                        // Ignored.
      &buffer,
      sizeof(buffer),
      &ignored,                 // Ignored.
      nullptr);                 // Ignored.

  if (!reparse_data_obtained) {
    return WindowsError(
        "'internal::windows::get_symbolic_link_data': 'DeviceIoControl' call "
        "failed");
  }

  return build_symbolic_link(buffer);
}


// Creates a reparse point with the specified target. The target can be either
// a file (in which case a junction is created), or a folder (in which case a
// mount point is created).
inline Try<Nothing> create_symbolic_link(
    const std::string& target,
    const std::string& reparse_point)
{
  // Determine if target is a folder or a file. This makes a difference
  // in the way we call `create_symbolic_link`.
  const Try<DWORD> attributes = get_file_attributes(longpath(target));

  bool target_is_folder = false;
  if (attributes.isSome()) {
    target_is_folder = attributes.get() & FILE_ATTRIBUTE_DIRECTORY;
  }

  // Bail out if target is already a reparse point.
  Try<bool> attribute_set = reparse_point_attribute_set(longpath(target));
  if (attribute_set.isSome() && attribute_set.get()) {
    return Error("Path '" + target + "' is already a reparse point");
  }

  DWORD flags = target_is_folder ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;

  // Lambda to create symlink with given flags.
  auto link = [&reparse_point, &target](const DWORD flags) {
    return ::CreateSymbolicLinkW(
        // Path to link.
        longpath(reparse_point).data(),
        // Path to target.
        longpath(target).data(),
        flags);
  };

  // `CreateSymbolicLink` normally adjusts the process token's privileges to
  // allow for symlink creation; however, we explicitly avoid this with the
  // following flag to not require administrative privileges.
  if (link(flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    return Nothing();
  }

  // If this failed because the non-symbolic link feature was not supported,
  // try again without the feature. This is for legacy support.
  if (::GetLastError() == ERROR_INVALID_PARAMETER) {
    if (link(flags)) {
      return Nothing();
    }
  }

  return WindowsError();
}

} // namespace windows {
} // namespace internal {

#endif // __STOUT_INTERNAL_WINDOWS_REPARSEPOINT_HPP__
