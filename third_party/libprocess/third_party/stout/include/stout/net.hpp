#ifndef __STOUT_NET_HPP__
#define __STOUT_NET_HPP__

#include <netdb.h>
#include <stdio.h>

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#include <string>

#include "error.hpp"
#include "os.hpp"
#include "try.hpp"


// Network utilities.
namespace net {

// Returns the HTTP response code resulting from attempting to download the
// specified HTTP or FTP URL into a file at the specified path.
inline Try<int> download(const std::string& url, const std::string& path)
{
#ifndef HAVE_LIBCURL
  return Error("libcurl is not available");
#else
  Try<int> fd = os::open(
      path, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    return Error(fd.error());
  }

  curl_global_init(CURL_GLOBAL_ALL);
  CURL* curl = curl_easy_init();

  if (curl == NULL) {
    curl_easy_cleanup(curl);
    os::close(fd.get());
    return Error("Failed to initialize libcurl");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);

  FILE* file = fdopen(fd.get(), "w");
  if (file == NULL) {
    return ErrnoError("Failed to open file handle of '" + path + "'");
  }
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

  CURLcode curlErrorCode = curl_easy_perform(curl);
  if (curlErrorCode != 0) {
    curl_easy_cleanup(curl);
    fclose(file);
    return Error(curl_easy_strerror(curlErrorCode));
  }

  long code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  if (fclose(file) != 0) {
    return ErrnoError("Failed to close file handle of '" + path + "'");
  }

  return Try<int>::some(code);
#endif // HAVE_LIBCURL
}

// Returns a Try of the hostname for the provided IP. If the hostname cannot
// be resolved, then a string version of the IP address is returned.
inline Try<std::string> getHostname(uint32_t ip)
{
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ip;

  char hostname[MAXHOSTNAMELEN];
  if (getnameinfo(
      (sockaddr*)&addr,
      sizeof(addr),
      hostname,
      MAXHOSTNAMELEN,
      NULL,
      0,
      0) != 0) {
    return ErrnoError();
  }

  return std::string(hostname);
}

} // namespace net {

#endif // __STOUT_NET_HPP__
