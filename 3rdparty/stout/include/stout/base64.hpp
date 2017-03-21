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

#ifndef __STOUT_BASE64_HPP__
#define __STOUT_BASE64_HPP__

#include <cctype>
#include <functional>
#include <string>

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

namespace base64 {

namespace internal {

// This slightly modified base64 implementation from
// cplusplus.com answer by modoran can be found at:
// http://www.cplusplus.com/forum/beginner/51572/

constexpr char STANDARD_CHARS[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz"
  "0123456789+/";

constexpr char URL_SAFE_CHARS[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz"
  "0123456789-_";


inline std::string encode(
    const std::string& s,
    const std::string& chars,
    bool padding)
{
  std::string result;
  int i = 0;
  int j = 0;
  unsigned char array3[3];
  unsigned char array4[4];
  const char* bytesToEncode = s.c_str();
  size_t length = s.size();

  while (length--) {
    array3[i++] = *(bytesToEncode++);
    if (i == 3) {
      array4[0] = (array3[0] & 0xfc) >> 2;
      array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
      array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
      array4[3] = array3[2] & 0x3f;
      for (i = 0; i < 4; i++) {
        result += chars[array4[i]];
      }
      i = 0;
    }
  }

  if (i != 0) {
    for (j = i; j < 3; j++) {
      array3[j] = '\0';
    }
    array4[0] = (array3[0] & 0xfc) >> 2;
    array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
    array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
    array4[3] = array3[2] & 0x3f;
    for (j = 0; j < i + 1; j++) {
      result += chars[array4[j]];
    }
    if (padding) {
      while (i++ < 3) {
        result += '=';
      }
    }
  }

  return result;
}


inline Try<std::string> decode(const std::string& s, const std::string& chars)
{
  auto isBase64 = [&chars](unsigned char c) -> bool {
    return (isalnum(c) || (c == chars[62]) || (c == chars[63]));
  };

  size_t i = 0;
  unsigned char array3[3];
  unsigned char array4[4];
  std::string result;

  foreach (unsigned char c, s) {
    if (c == '=') {
      // TODO(bmahler): Note that this does not validate that
      // there are the correct number of '=' characters!
      break; // Reached the padding.
    }

    if (!isBase64(c)) {
      return Error("Invalid character '" + stringify(c) + "'");
    }

    array4[i++] = c;

    if (i == 4) {
      for (i = 0; i < 4; i++) {
        array4[i] = static_cast<unsigned char>(chars.find(array4[i]));
      }
      array3[0] = (array4[0] << 2) + ((array4[1] & 0x30) >> 4);
      array3[1] = ((array4[1] & 0xf) << 4) + ((array4[2] & 0x3c) >> 2);
      array3[2] = ((array4[2] & 0x3) << 6) + array4[3];
      for (i = 0; i < 3; i++) {
        result += array3[i];
      }
      i = 0;
    }
  }

  if (i != 0) {
    size_t j;

    for (j = i; j < 4; j++) {
      array4[j] = 0;
    }
    for (j = 0; j < 4; j++) {
      array4[j] = static_cast<unsigned char>(chars.find(array4[j]));
    }
    array3[0] = (array4[0] << 2) + ((array4[1] & 0x30) >> 4);
    array3[1] = ((array4[1] & 0xf) << 4) + ((array4[2] & 0x3c) >> 2);
    array3[2] = ((array4[2] & 0x3) << 6) + array4[3];
    for (j = 0; (j < i - 1); j++) {
      result += array3[j];
    }
  }

  return result;
}

} // namespace internal {


/**
 * Encode a string to Base64 with the standard Base64 alphabet.
 * @see <a href="https://tools.ietf.org/html/rfc4648#section-4">RFC4648</a>
 *
 * @param s The string to encode.
 */
inline std::string encode(const std::string& s)
{
  return internal::encode(s, internal::STANDARD_CHARS, true);
}


/**
 * Decode a string that is Base64-encoded with the standard Base64
 * alphabet.
 * @see <a href="https://tools.ietf.org/html/rfc4648#section-4">RFC4648</a>
 *
 * @param s The string to decode.
 */
inline Try<std::string> decode(const std::string& s)
{
  return internal::decode(s, internal::STANDARD_CHARS);
}


/**
 * Encode a string to Base64 with a URL and filename safe alphabet.
 * @see <a href="https://tools.ietf.org/html/rfc4648#section-5">RFC4648</a>
 *
 * @param s The string to encode.
 * @param padding True if padding characters ('=') should be added.
 */
inline std::string encode_url_safe(const std::string& s, bool padding = true)
{
  return internal::encode(s, internal::URL_SAFE_CHARS, padding);
}


/**
 * Decode a string that is Base64-encoded with a URL and filename safe
 * alphabet.
 * @see <a href="https://tools.ietf.org/html/rfc4648#section-5">RFC4648</a>
 *
 * @param s The string to decode.
 */
inline Try<std::string> decode_url_safe(const std::string& s)
{
  return internal::decode(s, internal::URL_SAFE_CHARS);
}

} // namespace base64 {

#endif // __STOUT_BASE64_HPP__
