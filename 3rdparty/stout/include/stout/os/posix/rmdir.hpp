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

#ifndef __STOUT_OS_POSIX_RMDIR_HPP__
#define __STOUT_OS_POSIX_RMDIR_HPP__

#include <fts.h>
#include <unistd.h>
#include <string>

#include <glog/logging.h>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include <stout/os/exists.hpp>


namespace os {

// By default, recursively deletes a directory akin to: 'rm -r'. If
// `recursive` is false, it deletes a directory akin to: 'rmdir'. In
// recursive mode, `removeRoot` can be set to false to enable removing
// all the files and directories beneath the given root directory, but
// not the root directory itself.
// Note that this function expects an absolute path.
// By default rmdir aborts when an error occurs during the deletion of
// any file but if 'continueOnError' is set to true, rmdir logs the error
// and continues with the next file.
#ifndef __sun // FTS is not available on Solaris.
inline Try<Nothing> rmdir(
    const std::string& directory,
    bool recursive = true,
    bool removeRoot = true,
    bool continueOnError = false)
{
  if (!recursive) {
    if (::rmdir(directory.c_str()) < 0) {
      return ErrnoError();
    }
  } else {
    // NOTE: `fts_open` will not always return `nullptr` if the path does not
    // exist. We manually induce an error here to indicate that we can't remove
    // a directory that does not exist.
    if (!os::exists(directory)) {
      return ErrnoError(ENOENT);
    }

    char* paths[] = {const_cast<char*>(directory.c_str()), nullptr};

    // Using `FTS_PHYSICAL` here because we need `FTSENT` for the
    // symbolic link in the directory and not the target it links to.
    FTS* tree = fts_open(paths, (FTS_NOCHDIR | FTS_PHYSICAL), nullptr);
    if (tree == nullptr) {
      return ErrnoError();
    }

    FTSENT* node;
    while ((node = fts_read(tree)) != nullptr) {
      switch (node->fts_info) {
        case FTS_DP:
          // Don't remove the root of the traversal of `removeRoot`
          // is false.
          if (!removeRoot && node->fts_level == FTS_ROOTLEVEL) {
            continue;
          }

          if (::rmdir(node->fts_path) < 0 && errno != ENOENT) {
            if (continueOnError) {
              LOG(ERROR) << "Failed to delete directory "
                         << path::join(directory, node->fts_path)
                         << ": " << os::strerror(errno);
            } else {
              Error error = ErrnoError();
              fts_close(tree);
              return error;
            }
          }
          break;
        // `FTS_DEFAULT` would include any file type which is not
        // explicitly described by any of the other `fts_info` values.
        case FTS_DEFAULT:
        case FTS_F:
        case FTS_SL:
        // `FTS_SLNONE` should never be the case as we don't set
        // `FTS_COMFOLLOW` or `FTS_LOGICAL`. Adding here for completion.
        case FTS_SLNONE:
          if (::unlink(node->fts_path) < 0 && errno != ENOENT) {
            if (continueOnError) {
              LOG(ERROR) << "Failed to delete path "
                         << path::join(directory, node->fts_path)
                         << ": " << os::strerror(errno);
            } else {
              Error error = ErrnoError();
              fts_close(tree);
              return error;
            }
          }
          break;
        default:
          break;
      }
    }

    // If 'continueOnError' is true, we have already logged individual errors.
    if (errno != 0) {
      Error error = continueOnError
        ? Error("rmdir failed in 'continueOnError' mode")
        : ErrnoError();
      fts_close(tree);
      return error;
    }

    if (fts_close(tree) < 0) {
      return ErrnoError();
    }
  }

  return Nothing();
}
#endif // __sun

} // namespace os {


#endif // __STOUT_OS_POSIX_RMDIR_HPP__
