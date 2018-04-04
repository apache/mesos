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

#ifndef __STOUT_NET_HPP__
#define __STOUT_NET_HPP__

#ifndef __WINDOWS__
#include <netdb.h>
#endif // __WINDOWS__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef __WINDOWS__
#include <arpa/inet.h>
#endif // __WINDOWS__

#ifdef __APPLE__
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif // __APPLE__

#ifdef __FreeBSD__
#include <ifaddrs.h>
#endif // __FreeBSD__

// Note: Header grouping and ordering is considered before
// inclusion/exclusion by platform.
#ifndef __WINDOWS__
#include <sys/param.h>
#endif // __WINDOWS__

#include <curl/curl.h>

#include <iostream>
#include <set>
#include <string>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/ip.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/close.hpp>
#include <stout/os/open.hpp>

#ifdef __WINDOWS__
#include <stout/windows/net.hpp>
#else
#include <stout/posix/net.hpp>
#endif // __WINDOWS__


// Network utilities.
namespace net {

// Initializes libraries that net:: functions depend on, in a
// thread-safe way. This does not have to be called explicitly by
// the user of any functions in question. They will call this
// themselves by need.
inline void initialize()
{
  // We use a static struct variable to initialize in a thread-safe
  // way, at least with respect to calls within net::*, since there
  // is no way to guarantee that another library is not concurrently
  // initializing CURL. Thread safety is provided by the fact that
  // the value 'curl' should get constructed (i.e., the CURL
  // constructor invoked) in a thread safe way (as of GCC 4.3 and
  // required for C++11).
  struct CURL
  {
    CURL()
    {
      // This is the only one function in libcurl that is not deemed
      // thread-safe. (And it must be called at least once before any
      // other libcurl function is used.)
      curl_global_init(CURL_GLOBAL_ALL);
    }
  };

  static CURL curl;
}


// Downloads the header of the specified HTTP URL with a HEAD request
// and queries its "content-length" field. (Note that according to the
// HTTP specification there is no guarantee that this field contains
// any useful value.)
inline Try<Bytes> contentLength(const std::string& url)
{
  initialize();

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    curl_easy_cleanup(curl);
    return Error("Failed to initialize libcurl");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);
  curl_easy_setopt(curl, CURLOPT_HEADER, 1);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1);

  CURLcode curlErrorCode = curl_easy_perform(curl);
  if (curlErrorCode != 0) {
    curl_easy_cleanup(curl);
    return Error(curl_easy_strerror(curlErrorCode));
  }

  double result;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &result);

  curl_easy_cleanup(curl);

  if (result < 0) {
    return Error("No URL content-length available");
  }

  return Bytes(uint64_t(result));
}


// Returns the HTTP response code resulting from attempting to
// download the specified HTTP or FTP URL into a file at the specified
// path. The `stall_timeout` parameter controls how long the download
// waits before aborting when the download speed keeps below 1 byte/sec.
inline Try<int> download(
    const std::string& url,
    const std::string& path,
    const Option<Duration>& stall_timeout = None())
{
  initialize();

  Try<int_fd> fd = os::open(
      path,
      O_CREAT | O_WRONLY | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return Error(fd.error());
  }

  CURL* curl = curl_easy_init();

  if (curl == nullptr) {
    curl_easy_cleanup(curl);
    os::close(fd.get());
    return Error("Failed to initialize libcurl");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);

  // We don't bother introducing a `os::fdopen()` since this is the
  // only place we use `fdopen()` in the entire codebase as of writing
  // this comment.
#ifdef __WINDOWS__
  // This explicitly allocates a CRT integer file descriptor, which
  // when closed, also closes the underlying handle, so we do not call
  // `CloseHandle()` (or `os::close()`).
  const int crt = fd->crt();
  // We open in "binary" mode on Windows to avoid line-ending translation.
  FILE* file = ::_fdopen(crt, "wb");
  if (file == nullptr) {
    curl_easy_cleanup(curl);
    // NOTE: This is not `os::close()` because we allocated a CRT int
    // fd earlier.
    ::_close(crt);
    return ErrnoError("Failed to open file handle of '" + path + "'");
  }
#else
  FILE* file = ::fdopen(fd.get(), "w");
  if (file == nullptr) {
    curl_easy_cleanup(curl);
    os::close(fd.get());
    return ErrnoError("Failed to open file handle of '" + path + "'");
  }
#endif // __WINDOWS__

  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

  if (stall_timeout.isSome()) {
    // Set the options to abort the download if the speed keeps below
    // 1 byte/sec during the timeout. See:
    // https://curl.haxx.se/libcurl/c/CURLOPT_LOW_SPEED_LIMIT.html
    // https://curl.haxx.se/libcurl/c/CURLOPT_LOW_SPEED_TIME.html
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(
        curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(stall_timeout->secs()));
  }

  CURLcode curlErrorCode = curl_easy_perform(curl);
  if (curlErrorCode != 0) {
    curl_easy_cleanup(curl);
    // NOTE: `fclose()` also closes the associated file descriptor, so
    // we do not call `close()`.
    ::fclose(file);
    return Error(curl_easy_strerror(curlErrorCode));
  }

  long code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  if (::fclose(file) != 0) {
    return ErrnoError("Failed to close file handle of '" + path + "'");
  }

  return Try<int>::some(code);
}

} // namespace net {

#endif // __STOUT_NET_HPP__
