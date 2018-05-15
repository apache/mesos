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

#define LIBARCHIVE_STATIC

#include <archive.h>
#include <archive_entry.h>

#include <glog/logging.h>

#include <stout/nothing.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/open.hpp>
#include <stout/os/close.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

namespace archiver {

// Flags can be any of (or together):
//   ARCHIVE_EXTRACT_ACL
//   ARCHIVE_EXTRACT_FFLAGS
//   ARCHIVE_EXTRACT_PERM
//   ARCHIVE_EXTRACT_TIME

static bool handleError(const int result,
                        struct archive *a, Try<Nothing> &returnStatus)
{
  if (result < ARCHIVE_OK) {
    LOG(WARNING) << archive_error_string(a);
    if (returnStatus.isSome()) {
      returnStatus = Error("Archive extraction failed due to prior errors");
    }
  }
  if (result < ARCHIVE_WARN)
  {
    returnStatus = Error(archive_error_string(a));
    return true;
  }
  return false;
}

inline Try<Nothing> extract(
    const std::string& source,
    const std::string& destination,
    const int flags = ARCHIVE_EXTRACT_TIME)
{
  Try<Nothing> returnStatus = Nothing();

  // Get references to libarchive for reading/handling a compressed file.

  std::unique_ptr<struct archive, std::function<void(struct archive*)>> reader(
      archive_read_new(),
      [](struct archive* p) { archive_read_close(p);  archive_read_free(p); });

  archive_read_support_format_all(reader.get());
  archive_read_support_filter_all(reader.get());

  std::unique_ptr<struct archive, std::function<void(struct archive*)>> writer(
      archive_write_disk_new(),
      [](struct archive* p) { archive_write_close(p); archive_write_free(p); });

  archive_write_disk_set_options(writer.get(), flags);
  archive_write_disk_set_standard_lookup(writer.get());

  // Open the compressed file for decompression.
  //
  // We do not use libarchive to open the file to insure we have proper
  // Unicode and long path handling on both Linux and Windows.
  Try<int_fd> fd = os::open(source, O_RDONLY | O_CLOEXEC, 0);

  if (fd.isError()) {
    return Error("Failed to open file '" + source + "': " + fd.error());
  }

#ifdef __WINDOWS__
  int fd_real = fd.get().crt();
#else
  int fd_real = fd.get();
#endif // __WINDOWS__

  int result = archive_read_open_fd(
      reader.get(),
      fd_real,
      10240);
  if (result) {
    os::close(fd.get());
    return Error(archive_error_string(reader.get()));
  }

  // Loop through file headers in the archive stream.
  while (true) {
    // Read the next header from the input stream.
    struct archive_entry *entry;
    result = archive_read_next_header(reader.get(), &entry);
    if (result == ARCHIVE_EOF) {
      break;
    }
    if (handleError(result, reader.get(), returnStatus)) {
      os::close(fd.get());
      return returnStatus;
    }

    // If a destination path is specified, update the entry to reflect it.
    // We assume the destination directory already exists.

    if (!destination.empty()) {
      std::string path = path::join(destination,
          archive_entry_pathname(entry));
      archive_entry_update_pathname_utf8(entry, path.c_str());
    }

    result = archive_write_header(writer.get(), entry);
    if (handleError(result, writer.get(), returnStatus)) {
      os::close(fd.get());
      return returnStatus;
    }

    if (archive_entry_size(entry) > 0) {
      const void *buff;
      size_t size;
#if ARCHIVE_VERSION_NUMBER >= 3000000
      int64_t offset;
#else
      off_t offset;
#endif

      // Loop through file data blocks until end of file.
      while (true) {
        result = archive_read_data_block(
            reader.get(),
            &buff,
            &size,
            &offset);
        if (result == ARCHIVE_EOF) {
          break;
        }
        if (handleError(result, reader.get(), returnStatus)) {
          break;
        }

        result = archive_write_data_block(writer.get(), buff, size, offset);
        if (handleError(result, writer.get(), returnStatus)) {
          break;
        }
      }

      // If an error happened dealing with file contents, abort processing.
      if (result < ARCHIVE_WARN) {
        break;
      }
    }

    result = archive_write_finish_entry(writer.get());
    if (handleError(result, writer.get(), returnStatus)) {
      break;
    }
  }

  os::close(fd.get());
  return returnStatus;
}

} // namespace archiver {

#endif // __STOUT_ARCHIVER_HPP__
