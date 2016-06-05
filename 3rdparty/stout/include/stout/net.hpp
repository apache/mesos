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
#include <stout/error.hpp>
#include <stout/ip.hpp>
#ifdef __WINDOWS__
#include <stout/windows/net.hpp>
#else
#include <stout/posix/net.hpp>
#endif // __WINDOWS__
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/open.hpp>


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
// path.
inline Try<int> download(const std::string& url, const std::string& path)
{
  initialize();

  Try<int> fd = os::open(
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

  FILE* file = fdopen(fd.get(), "w");
  if (file == nullptr) {
    curl_easy_cleanup(curl);
    os::close(fd.get());
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
}


inline struct addrinfo createAddrInfo(int socktype, int family, int flags)
{
  struct addrinfo addr;
  memset(&addr, 0, sizeof(addr));
  addr.ai_socktype = socktype;
  addr.ai_family = family;
  addr.ai_flags |= flags;

  return addr;
}


// TODO(evelinad): Move this to Address.
inline struct sockaddr_storage createSockaddrStorage(const IP& ip, int port)
{
  struct sockaddr_storage storage;
  memset(&storage, 0, sizeof(storage));

  switch (ip.family()) {
    case AF_INET: {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr = ip.in().get();
      addr.sin_port = htons(port);

      memcpy(&storage, &addr, sizeof(addr));
      break;
    }
    default: {
      ABORT("Unsupported family type: " + stringify(ip.family()));
    }
  }

  return storage;
}


inline Try<std::string> hostname()
{
  char host[512];

  if (gethostname(host, sizeof(host)) < 0) {
    return ErrnoError();
  }

  // TODO(evelinad): Add AF_UNSPEC when we will support IPv6.
  struct addrinfo hints = createAddrInfo(SOCK_STREAM, AF_INET, AI_CANONNAME);
  struct addrinfo* result = nullptr;

  int error = getaddrinfo(host, nullptr, &hints, &result);

  if (error != 0) {
    return Error(gai_strerror(error));
  }

  std::string hostname = result->ai_canonname;
  freeaddrinfo(result);

  return hostname;
}


// Returns a Try of the hostname for the provided IP. If the hostname
// cannot be resolved, then a string version of the IP address is
// returned.
inline Try<std::string> getHostname(const IP& ip)
{
  struct sockaddr_storage storage = createSockaddrStorage(ip, 0);
  char hostname[MAXHOSTNAMELEN];

  int error = getnameinfo(
      (struct sockaddr*) &storage,
#ifdef __FreeBSD__
      sizeof(struct sockaddr_in),
#else
      sizeof(storage),
#endif
      hostname,
      MAXHOSTNAMELEN,
      nullptr,
      0,
      0);

  if (error != 0) {
    return Error(std::string(gai_strerror(error)));
  }

  return std::string(hostname);
}


// Returns the names of all the link devices in the system.
inline Try<std::set<std::string>> links()
{
#if !defined(__linux__) && !defined(__APPLE__) && !defined(__FreeBSD__)
  return Error("Not implemented");
#else
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  std::set<std::string> names;
  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != nullptr) {
      names.insert(ifa->ifa_name);
    }
  }

  freeifaddrs(ifaddr);
  return names;
#endif
}


// Returns a Try of the IP for the provided hostname or an error if no IP is
// obtained.
inline Try<IP> getIP(const std::string& hostname, int family)
{
  struct addrinfo hints = createAddrInfo(SOCK_STREAM, family, 0);
  struct addrinfo* result = nullptr;

  int error = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);

  if (error != 0) {
    return Error(gai_strerror(error));
  }

  if (result->ai_addr == nullptr) {
    freeaddrinfo(result);
    return Error("No addresses found");
  }

  Try<IP> ip = IP::create(*result->ai_addr);

  if (ip.isError()) {
    freeaddrinfo(result);
    return Error("Unsupported family type");
  }

  freeaddrinfo(result);
  return ip.get();
}

} // namespace net {

#endif // __STOUT_NET_HPP__
