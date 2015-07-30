/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_WINDOWS_NET_HPP__
#define __STOUT_WINDOWS_NET_HPP__

#include <string>

#include <stout/ip.hpp>
#include <stout/try.hpp>


// Network utilities.
namespace net {

// Downloads the header of the specified HTTP URL with a HEAD request
// and queries its "content-length" field. (Note that according to the
// HTTP specification there is no guarantee that this field contains
// any useful value.)
inline Try<Bytes> contentLength(const std::string& url)
{
  UNIMPLEMENTED;
}


// Returns the HTTP response code resulting from attempting to
// download the specified HTTP or FTP URL into a file at the specified
// path.
inline Try<int> download(const std::string& url, const std::string& path)
{
  UNIMPLEMENTED;
}

} // namespace net {

#endif // __STOUT_WINDOWS_NET_HPP__
