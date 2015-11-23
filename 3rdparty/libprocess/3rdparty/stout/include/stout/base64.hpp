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

// This slightly modified base64 implementation from
// cplusplus.com answer by modoran can be found at:
// http://www.cplusplus.com/forum/beginner/51572/

static const std::string chars =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz"
  "0123456789+/";


inline std::string encode(const std::string& s)
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
    while (i++ < 3) {
      result += '=';
    }
  }

  return result;
}


inline Try<std::string> decode(const std::string& s)
{
  auto isBase64 = [](unsigned char c) -> bool {
    return (isalnum(c) || (c == '+') || (c == '/'));
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

} // namespace base64 {

#endif // __STOUT_BASE64_HPP__
