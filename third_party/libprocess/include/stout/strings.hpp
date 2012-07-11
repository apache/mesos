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

#ifndef __STOUT_STRINGS_HPP__
#define __STOUT_STRINGS_HPP__

#include <string>
#include <map>
#include <vector>

#include "foreach.hpp"
#include "format.hpp" // TODO(benh): Include format.hpp explicitly.

namespace strings {

// Flags indicating how remove should operate.
enum Mode {
  PREFIX,
  SUFFIX,
  ANY
};


inline std::string remove(
    const std::string& from,
    const std::string& substring,
    Mode mode = ANY)
{
  std::string result = from;

  if (mode == PREFIX) {
    if (from.find(substring) == 0) {
      result = from.substr(substring.size());
    }
  } else if (mode == SUFFIX) {
    if (from.rfind(substring) == from.size() - substring.size()) {
      result = from.substr(0, from.size() - substring.size());
    }
  } else {
    size_t index;
    while ((index = result.find(substring)) != std::string::npos) {
      result = result.erase(index, substring.size());
    }
  }

  return result;
}


inline std::string trim(
    const std::string& from,
    const std::string& chars = " \t\n\r")
{
  size_t start = from.find_first_not_of(chars);
  size_t end = from.find_last_not_of(chars);
  if (start == std::string::npos) { // Contains only characters in chars.
    return "";
  }

  return from.substr(start, end + 1 - start);
}


inline std::vector<std::string> split(
    const std::string& s,
    const std::string& delims)
{
  std::vector<std::string> tokens;

  size_t offset = 0;
  while (true) {
    size_t i = s.find_first_not_of(delims, offset);
    if (std::string::npos == i) {
      offset = s.length();
      return tokens;
    }

    size_t j = s.find_first_of(delims, i);
    if (std::string::npos == j) {
      tokens.push_back(s.substr(i));
      offset = s.length();
      continue;
    }

    tokens.push_back(s.substr(i, j - i));
    offset = j;
  }
}


// Returns a map of strings to strings based on calling split
// twice. For example:
//
//   pairs("foo=1;bar=2;foo=3", ";&", "=")
//
// Would return a map with the following:
//   foo: [1, 3]
//   bar: [2]
inline std::map<std::string, std::vector<std::string> > pairs(
    const std::string& s,
    const std::string& delims1,
    const std::string& delims2)
{
  std::map<std::string, std::vector<std::string> > result;

  const std::vector<std::string>& tokens = split(s, delims1);
  foreach (const std::string& token, tokens) {
    const std::vector<std::string>& pairs = split(token, delims2);
    if (pairs.size() == 2) {
      result[pairs[0]].push_back(pairs[1]);
    }
  }

  return result;
}


inline bool checkBracketsMatching(
    const std::string& s,
    const char openBracket,
    const char closeBracket)
{
  int count = 0;
  for (int i = 0; i < s.length(); i++) {
    if (s[i] == openBracket) {
      count++;
    } else if (s[i] == closeBracket) {
      count--;
    }
    if (count < 0) {
      return false;
    }
  }
  return count == 0;
}


inline bool startsWith(const std::string& s, const std::string& prefix)
{
  return s.find(prefix) == 0;
}


inline bool endsWith(const std::string& s, const std::string& suffix)
{
  return s.rfind(suffix) == s.length() - suffix.length();
}


inline bool contains(const std::string& s, const std::string& substr)
{
  return s.find(substr) != std::string::npos;
}


} // namespaces strings {

#endif // __STOUT_STRINGS_HPP__
