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
#ifndef __STOUT_WINDOWS_GZIP_HPP__
#define __STOUT_WINDOWS_GZIP_HPP__

#include <string>

#include <stout/try.hpp>


namespace gzip {

// Returns a gzip compressed version of the provided string.
// The compression level should be within the range [-1, 9].
// See zlib.h:
//   #define Z_NO_COMPRESSION         0
//   #define Z_BEST_SPEED             1
//   #define Z_BEST_COMPRESSION       9
//   #define Z_DEFAULT_COMPRESSION  (-1)
inline Try<std::string> compress(
    const std::string& decompressed,
    int level = Z_DEFAULT_COMPRESSION)
{
  UNIMPLEMENTED;
}


// Returns a gzip decompressed version of the provided string.
inline Try<std::string> decompress(const std::string& compressed)
{
  UNIMPLEMENTED;
}

} // namespace gzip {

#endif // __STOUT_WINDOWS_GZIP_HPP__
