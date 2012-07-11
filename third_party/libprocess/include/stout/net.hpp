/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __STOUT_NET_HPP__
#define __STOUT_NET_HPP__

#include <stdio.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#include <string>

#include "os.hpp"
#include "try.hpp"


// Handles http requests.
namespace net {

// Returns the return code resulting from attempting to download the
// specified HTTP or FTP URL into a file at the specified path.
inline Try<int> download(const std::string& url, const std::string& path)
{
#ifndef HAVE_LIBCURL
  return Try<int>::error("Downloading via HTTP/FTP is not supported");
#else
  Try<int> fd = os::open(path, O_CREAT | O_WRONLY,
                         S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  CHECK(!fd.isNone());

  if (fd.isError()) {
    return Try<int>::error(fd.error());
  }

  curl_global_init(CURL_GLOBAL_ALL);
  CURL* curl = curl_easy_init();

  if (curl == NULL) {
    curl_easy_cleanup(curl);
    os::close(fd.get());
    return Try<int>::error("Failed to initialize libcurl");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);

  FILE* file = fdopen(fd.get(), "w");
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

  CURLcode curlErrorCode = curl_easy_perform(curl);
  if (curlErrorCode != 0) {
    curl_easy_cleanup(curl);
    fclose(file);
    return Try<int>::error(curl_easy_strerror(curlErrorCode));
  }

  long code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  if (!fclose(file)) {
    return Try<int>::error("Failed to close file handle");
  }

  return Try<int>::some(code);
#endif // HAVE_LIBCURL
}


} // namespace net {

#endif // __STOUT_NET_HPP__
