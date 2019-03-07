// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_ARCHIVER_HPP__
#define __STOUT_ARCHIVER_HPP__

#include <archive.h>
#include <archive_entry.h>

#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include <stout/os/close.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/open.hpp>

namespace archiver {

// Extracts the archive in source to the destination folder (if specified).
// If destination is not specified, it will use the working directory.
// Flags can be any of (or together):
//   ARCHIVE_EXTRACT_ACL
//   ARCHIVE_EXTRACT_FFLAGS
//   ARCHIVE_EXTRACT_PERM
//   ARCHIVE_EXTRACT_TIME
inline Try<Nothing> extract(
  const std::string& source,
  const std::string& destination,
  const int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_NODOTDOT)
{
  // Get references to libarchive for reading/handling a compressed file.
  std::unique_ptr<struct archive, std::function<void(struct archive*)>> reader(
    archive_read_new(),
    [](struct archive* p) {
      archive_read_close(p);
      archive_read_free(p);
    });

  // Enable auto-detection of the archive type/format.
  archive_read_support_format_all(reader.get());
  archive_read_support_filter_all(reader.get());

  std::unique_ptr<struct archive, std::function<void(struct archive*)>> writer(
    archive_write_disk_new(),
    [](struct archive* p) {
      archive_write_close(p);
      archive_write_free(p);
    });

  archive_write_disk_set_options(writer.get(), flags);
  archive_write_disk_set_standard_lookup(writer.get());

  // Open the compressed file for decompression.
  //
  // We do not use libarchive to open the file to ensure we have proper
  // file descriptor and long path handling on both Posix and Windows.
  Try<int_fd> fd = os::open(source, O_RDONLY | O_CLOEXEC);
  if (fd.isError()) {
    return Error(fd.error());
  }

#ifdef __WINDOWS__
  int fd_real = fd->crt();
#else
  int fd_real = fd.get();
#endif

  // Ensure the CRT file descriptor is closed when leaving scope.
  // NOTE: On Windows, we need to explicitly allocate a CRT file descriptor
  // because libarchive requires it. Once the CRT fd is allocated, it must
  // be closed with _close instead of os::close.
  struct Closer
  {
    int fd_value;
#ifdef __WINDOWS__
    ~Closer() { ::_close(fd_value); }
#else
    ~Closer() { os::close(fd_value); }
#endif
  } closer = {fd_real};

  const size_t archive_block_size = 10240;
  int result = archive_read_open_fd(reader.get(), fd_real, archive_block_size);
  if (result != ARCHIVE_OK) {
    return Error(archive_error_string(reader.get()));
  }

  // Loop through file headers in the archive stream.
  while (true) {
    // Read the next header from the input stream.
    struct archive_entry* entry;
    result = archive_read_next_header(reader.get(), &entry);

    if (result == ARCHIVE_EOF) {
      break;
    } else if (result <= ARCHIVE_WARN) {
      return Error(
          std::string("Failed to read archive header: ") +
          archive_error_string(reader.get()));
    }

    // If a destination path is specified, update the entry to reflect it.
    // We assume the destination directory already exists.
    if (!destination.empty()) {
      // NOTE: This will be nullptr if the entry is not a hardlink.
      const char* hardlink_target = archive_entry_hardlink_utf8(entry);

      if (hardlink_target != nullptr) {
        archive_entry_update_hardlink_utf8(
            entry,
            path::join(destination, hardlink_target).c_str());
      }

      archive_entry_update_pathname_utf8(
          entry,
          path::join(destination, archive_entry_pathname_utf8(entry)).c_str());
    }

    result = archive_write_header(writer.get(), entry);
    if (result <= ARCHIVE_WARN) {
      return Error(
          std::string("Failed to write archive header: ") +
          archive_error_string(writer.get()));
    }

    if (archive_entry_size(entry) > 0) {
      const void* buff;
      size_t size;
#if ARCHIVE_VERSION_NUMBER >= 3000000
      int64_t offset;
#else
      off_t offset;
#endif

      // Loop through file data blocks until end of file.
      while (true) {
        result = archive_read_data_block(reader.get(), &buff, &size, &offset);
        if (result == ARCHIVE_EOF) {
          break;
        } else if (result <= ARCHIVE_WARN) {
          return Error(
              std::string("Failed to read archive data block: ") +
              archive_error_string(reader.get()));
        }

        result = archive_write_data_block(writer.get(), buff, size, offset);
        if (result <= ARCHIVE_WARN) {
          return Error(
              std::string("Failed to write archive data block: ") +
              archive_error_string(writer.get()));
        }
      }
    }

    result = archive_write_finish_entry(writer.get());
    if (result <= ARCHIVE_WARN) {
      return Error(
          std::string("Failed to write archive finish entry: ") +
          archive_error_string(writer.get()));
    }
  }

  return Nothing();
}

} // namespace archiver {

#endif // __STOUT_ARCHIVER_HPP__
