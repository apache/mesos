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

#include <stout/try.hpp>
#include <stout/windows.hpp>


namespace internal {
namespace windows {

// We pass this struct to `DeviceIoControl` to get information about a reparse
// point (including things like whether it's a symlink). It is normally part of
// the Device Driver Kit (DDK), specifically `nitfs.h`, but rather than taking
// a dependency on the DDK, we choose to simply copy the struct here. This is a
// well-worn path used by Boost FS[1], among others. See documentation
// here[2].
//
// [1] http://www.boost.org/doc/libs/1_46_1/libs/filesystem/v3/src/operations.cpp
// [2] https://msdn.microsoft.com/en-us/library/cc232007.aspx
typedef struct _REPARSE_DATA_BUFFER
{
  // Describes, among other things, which type of reparse point this is (e.g.,
  // a symlink).
  ULONG  ReparseTag;
  // Size in bytes of common portion of the `REPARSE_DATA_BUFFER`.
  USHORT  ReparseDataLength;
  // Unused. Ignore.
  USHORT  Reserved;
  union
  {
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
      // The first byte of the path string -- according to the documentation[1],
      // this is followed in memory by the rest of the path string. The "path
      // string" itself is a unicode char array containing both substitute name
      // and print name. They can occur in any order. Use the offset and length
      // of each in this struct to calculate where each starts and ends.
      //
      // [1] https://msdn.microsoft.com/en-us/library/windows/hardware/ff552012(v=vs.85).aspx
      WCHAR PathBuffer[1];
    } SymbolicLinkReparseBuffer;

    // Unused: holds mount point data.
    struct
    {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      WCHAR PathBuffer[1];
    } MountPointReparseBuffer;
    struct
    {
      UCHAR DataBuffer[1];
    } GenericReparseBuffer;
  };
} REPARSE_DATA_BUFFER;

#define REPARSE_MOUNTPOINT_HEADER_SIZE 8


// Convenience struct for holding symlink data, meant purely for internal use.
// We pass this around instead of the extremely inelegant `REPARSE_DATA_BUFFER`
// struct, simply because this struct is easier to deal with and reason about.
struct SymbolicLink
{
  std::wstring substitute_name;
  std::wstring print_name;
  ULONG flags;
};


// Checks file/folder attributes for a path to see if the reparse point
// attribute is set; this indicates whether the path points at a reparse point,
// rather than a "normal" file or folder.
inline Try<bool> reparse_point_attribute_set(const std::string& absolute_path)
{
  const DWORD attributes = GetFileAttributes(absolute_path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return WindowsError(
        "Failed to get attributes for file '" + absolute_path + "'");
  }

  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
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


// Attempts to get a file or folder handle for an absolute path, and does not
// follow symlinks. That is, if the path points at a symlink, the handle will
// refer to the symlink rather than the file or folder the symlink points at.
inline Try<SharedHandle> get_handle_no_follow(const std::string& absolute_path)
{
  struct _stat s;
  bool resolved_path_is_directory = false;
  if (::_stat(absolute_path.c_str(), &s) >= 0) {
    resolved_path_is_directory = S_ISDIR(s.st_mode);
  }

  // NOTE: The `CreateFile` documentation[1] tells us which flags we need to
  // invoke to open a handle that will point at the symlink instead of the
  // folder or file it points at. The short answer is that you need to be sure
  // to pass in `OPEN_EXISTING` and `FILE_FLAG_OPEN_REPARSE_POINT` to get a
  // handle for the symlink and not the file the symlink points to.
  //
  // Note also that `CreateFile` will appropriately generate a handle for
  // either a folder or a file (they are different!), as long as you
  // appropriately set a magic flag, `FILE_FLAG_BACKUP_SEMANTICS`. It is not
  // clear why, or what this flag means, the documentation[1] only says it's
  // necessary.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx
  const DWORD access_flags = resolved_path_is_directory
    ? (FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS)
    : FILE_FLAG_OPEN_REPARSE_POINT;

  const HANDLE handle = CreateFile(
    absolute_path.c_str(),
    0,              // Ignored.
    0,              // Ignored.
    NULL,           // Ignored.
    OPEN_EXISTING,  // Open existing symlink.
    access_flags,   // Open symlink, not the file it points to.
    NULL);          // Ignored.

  if (handle == INVALID_HANDLE_VALUE) {
    return WindowsError(
        "Could not open handle for symlink at path '" + absolute_path + "'");
  }

  return SharedHandle(handle, CloseHandle);
}


// Attempts to get the symlink data for a file or folder handle.
inline Try<SymbolicLink> get_symbolic_link_data(const HANDLE handle)
{
  // To get the symlink data, we call `DeviceIoControl`. This function is part
  // of the Device Driver Kit (DDK), and buried away in a corner of the
  // DDK documentation[1] is an incomplete explanation of how to twist API to
  // get it to emit information about reparse points (and, thus, symlinks,
  // since symlinks are implemented with reparse points). This technique is a
  // hack, but it is used pretty much everywhere, including the Boost FS code,
  // though it's worth noting that they seem to use it incorrectly[2].
  //
  // Summarized, the documentation tells us that we need to pass in the magic
  // flag `FSCTL_GET_REPARSE_POINT` to get the function to populate a
  // `REPARSE_DATA_BUFFER` struct with data about a reparse point. What it
  // doesn't tell you is that this struct is in a header in the DDK, so in
  // order to get information about the reparse point, you must either take a
  // dependency on the DDK, or copy the struct from the header into your code.
  // We take a cue from Boost FS, and copy the struct into this header (see
  // above).
  //
  // Finally, for context, it may be worth looking at the (sparse)
  // documentation for `DeviceIoControl` itself.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa364571(v=vs.85).aspx
  // [2] https://svn.boost.org/trac/boost/ticket/4663
  // [3] https://msdn.microsoft.com/en-us/library/windows/desktop/aa363216(v=vs.85).aspx

  const size_t reparse_point_data_size = MAXIMUM_REPARSE_DATA_BUFFER_SIZE;
  BYTE buffer[reparse_point_data_size];
  REPARSE_DATA_BUFFER* reparse_point_data =
    reinterpret_cast<REPARSE_DATA_BUFFER*>(buffer);

  DWORD ignored = 0;

  // The semantics of this function are: get the reparse data associated with
  // the `handle` of some open directory or file, and that data in
  // `reparse_point_data`.
  const BOOL reparse_data_obtained = DeviceIoControl(
    handle,                  // handle to file or directory
    FSCTL_GET_REPARSE_POINT, // Gets reparse point data for file/folder handle.
    NULL,                    // Ignored.
    0,                       // Ignored.
    reparse_point_data,
    reparse_point_data_size,
    &ignored,                // Ignored.
    NULL);                   // Ignored.

  if (!reparse_data_obtained) {
    return WindowsError("Failed to obtain reparse point data for handle");
  }

  return build_symbolic_link(*reparse_point_data);
}

} // namespace windows {
} // namespace internal {

#endif // __STOUT_INTERNAL_WINDOWS_REPARSEPOINT_HPP__
