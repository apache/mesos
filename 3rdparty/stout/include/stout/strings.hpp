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

#ifndef __STOUT_STRINGS_HPP__
#define __STOUT_STRINGS_HPP__

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "foreach.hpp"
#include "format.hpp"
#include "option.hpp"
#include "stringify.hpp"

namespace strings {

const std::string WHITESPACE = " \t\n\r";

// Flags indicating how 'remove' or 'trim' should operate.
enum Mode
{
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
    Mode mode = ANY,
    const std::string& chars = WHITESPACE)
{
  size_t start = 0;
  Option<size_t> end = None();

  if (mode == ANY) {
    start = from.find_first_not_of(chars);
    end = from.find_last_not_of(chars);
  } else if (mode == PREFIX) {
    start = from.find_first_not_of(chars);
  } else if (mode == SUFFIX) {
    end = from.find_last_not_of(chars);
  }

  // Bail early if 'from' contains only characters in 'chars'.
  if (start == std::string::npos) {
    return "";
  }

  // Calculate the length of the substring, defaulting to the "end" of
  // string if there were no characters to remove from the suffix.
  size_t length = std::string::npos;

  // Found characters to trim at the end.
  if (end.isSome() && end.get() != std::string::npos) {
    length = end.get() + 1 - start;
  }

  return from.substr(start, length);
}


// Helper providing some syntactic sugar for when 'mode' is ANY but
// the 'chars' are specified.
inline std::string trim(
    const std::string& from,
    const std::string& chars)
{
  return trim(from, ANY, chars);
}


// Replaces all the occurrences of the 'from' string with the 'to' string.
inline std::string replace(
    const std::string& s,
    const std::string& from,
    const std::string& to)
{
  std::string result = s;
  size_t index = 0;

  if (from.empty()) {
    return result;
  }

  while ((index = result.find(from, index)) != std::string::npos) {
    result.replace(index, from.length(), to);
    index += to.length();
  }
  return result;
}


// Tokenizes the string using the delimiters. Empty tokens will not be
// included in the result.
//
// Optionally, the maximum number of tokens to be returned can be
// specified. If the maximum number of tokens is reached, the last
// token returned contains the remainder of the input string.
inline std::vector<std::string> tokenize(
    const std::string& s,
    const std::string& delims,
    const Option<size_t>& maxTokens = None())
{
  if (maxTokens.isSome() && maxTokens.get() == 0) {
    return {};
  }

  std::vector<std::string> tokens;
  size_t offset = 0;

  while (true) {
    size_t nonDelim = s.find_first_not_of(delims, offset);

    if (nonDelim == std::string::npos) {
      break; // Nothing left.
    }

    size_t delim = s.find_first_of(delims, nonDelim);

    // Finish tokenizing if this is the last token,
    // or we've found enough tokens.
    if (delim == std::string::npos ||
        (maxTokens.isSome() && tokens.size() == maxTokens.get() - 1)) {
      tokens.push_back(s.substr(nonDelim));
      break;
    }

    tokens.push_back(s.substr(nonDelim, delim - nonDelim));
    offset = delim;
  }

  return tokens;
}


// Splits the string using the provided delimiters. The string is
// split each time at the first character that matches any of the
// characters specified in delims.  Empty tokens are allowed in the
// result.
//
// Optionally, the maximum number of tokens to be returned can be
// specified. If the maximum number of tokens is reached, the last
// token returned contains the remainder of the input string.
inline std::vector<std::string> split(
    const std::string& s,
    const std::string& delims,
    const Option<size_t>& maxTokens = None())
{
  if (maxTokens.isSome() && maxTokens.get() == 0) {
    return {};
  }

  std::vector<std::string> tokens;
  size_t offset = 0;

  while (true) {
    size_t next = s.find_first_of(delims, offset);

    // Finish splitting if this is the last token,
    // or we've found enough tokens.
    if (next == std::string::npos ||
        (maxTokens.isSome() && tokens.size() == maxTokens.get() - 1)) {
      tokens.push_back(s.substr(offset));
      break;
    }

    tokens.push_back(s.substr(offset, next - offset));
    offset = next + 1;
  }

  return tokens;
}


// Returns a map of strings to strings based on calling tokenize
// twice. All non-pairs are discarded. For example:
//
//   pairs("foo=1;bar=2;baz;foo=3;bam=1=2", ";&", "=")
//
// Would return a map with the following:
//   bar: ["2"]
//   foo: ["1", "3"]
inline std::map<std::string, std::vector<std::string>> pairs(
    const std::string& s,
    const std::string& delims1,
    const std::string& delims2)
{
  std::map<std::string, std::vector<std::string>> result;

  const std::vector<std::string> tokens = tokenize(s, delims1);
  foreach (const std::string& token, tokens) {
    const std::vector<std::string> pairs = tokenize(token, delims2);
    if (pairs.size() == 2) {
      result[pairs[0]].push_back(pairs[1]);
    }
  }

  return result;
}


namespace internal {

inline std::stringstream& append(
    std::stringstream& stream,
    const std::string& value)
{
  stream << value;
  return stream;
}


inline std::stringstream& append(
    std::stringstream& stream,
    std::string&& value)
{
  stream << value;
  return stream;
}


inline std::stringstream& append(
    std::stringstream& stream,
    const char*&& value)
{
  stream << value;
  return stream;
}


template <typename T>
std::stringstream& append(
    std::stringstream& stream,
    T&& value)
{
  stream << ::stringify(std::forward<T>(value));
  return stream;
}


template <typename T>
std::stringstream& join(
    std::stringstream& stream,
    const std::string& separator,
    T&& tail)
{
  return append(stream, std::forward<T>(tail));
}


template <typename THead, typename... TTail>
std::stringstream& join(
    std::stringstream& stream,
    const std::string& separator,
    THead&& head,
    TTail&&... tail)
{
  append(stream, std::forward<THead>(head)) << separator;
  internal::join(stream, separator, std::forward<TTail>(tail)...);
  return stream;
}

} // namespace internal {


template <typename... T>
std::stringstream& join(
    std::stringstream& stream,
    const std::string& separator,
    T&&... args)
{
  internal::join(stream, separator, std::forward<T>(args)...);
  return stream;
}


// Use 2 heads here to disambiguate variadic argument join from the
// templatized Iterable join below. This means this implementation of
// strings::join() is only activated if there are 2 or more things to
// join.
template <typename THead1, typename THead2, typename... TTail>
std::string join(
    const std::string& separator,
    THead1&& head1,
    THead2&& head2,
    TTail&&... tail)
{
  std::stringstream stream;
  internal::join(
      stream,
      separator,
      std::forward<THead1>(head1),
      std::forward<THead2>(head2),
      std::forward<TTail>(tail)...);
  return stream.str();
}


// Ensure std::string doesn't fall into the iterable case
inline std::string join(const std::string& seperator, const std::string& s) {
  return s;
}


// Use duck-typing to join any iterable.
template <typename Iterable>
inline std::string join(const std::string& separator, const Iterable& i)
{
  std::string result;
  typename Iterable::const_iterator iterator = i.begin();
  while (iterator != i.end()) {
    result += stringify(*iterator);
    if (++iterator != i.end()) {
      result += separator;
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
  for (size_t i = 0; i < s.length(); i++) {
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
  return s.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), s.begin());
}


inline bool startsWith(const std::string& s, const char* prefix)
{
  size_t len = ::strnlen(prefix, s.size() + 1);
  return s.size() >= len &&
         std::equal(s.begin(), s.begin() + len, prefix);
}


inline bool startsWith(const std::string& s, char c)
{
  return !s.empty() && s.front() == c;
}


inline bool endsWith(const std::string& s, const std::string& suffix)
{
  return s.size() >= suffix.size() &&
         std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}


inline bool endsWidth(const std::string& s, const char* suffix)
{
  size_t len = ::strnlen(suffix, s.size() + 1);
  return s.size() >= len &&
         std::equal(s.end() - len, s.end(), suffix);
}


inline bool endsWith(const std::string& s, char c)
{
  return !s.empty() && s.back() == c;
}


inline bool contains(const std::string& s, const std::string& substr)
{
  return s.find(substr) != std::string::npos;
}


inline std::string lower(const std::string& s)
{
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(), ::tolower);
  return result;
}


inline std::string upper(const std::string& s)
{
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(), ::toupper);
  return result;
}

} // namespace strings {

#endif // __STOUT_STRINGS_HPP__
