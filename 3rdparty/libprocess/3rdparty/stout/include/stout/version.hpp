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
#ifndef __STOUT_VERSION_HPP__
#define __STOUT_VERSION_HPP__

#include <ostream>
#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/numify.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

// This class provides convenience routines for version checks.
// TODO(karya): Consider adding support for more than 3 components,
// and compatibility operators.
// TODO(karya): Add support for labels and build metadata. Consider
// semantic versioning (http://semvar.org/) for specs.
class Version
{
public:
  // Expect the string in the following format:
  //   <major>[.<minor>[.<patch>]]
  // Missing components are treated as zero.
  static Try<Version> parse(const std::string& s)
  {
    const size_t maxComponents = 3;

    // Use only the part before '-', i.e. strip and discard the tags
    // and labels.
    // TODO(karya): Once we have support for labels and tags, we
    // should not discard the remaining string.
    std::vector<std::string> split =
      strings::split(strings::split(s, "-")[0], ".");

    if (split.size() > maxComponents) {
      return Error("Version string has " + stringify(split.size()) +
                   " components; maximum " + stringify(maxComponents) +
                   " components allowed");
    }

    int components[maxComponents] = {0};

    for (size_t i = 0; i < split.size(); i++) {
      Try<int> result = numify<int>(split[i]);
      if (result.isError()) {
        return Error("Invalid version component '" + split[i] + "': " +
                     result.error());
      }
      components[i] = result.get();
    }

    return Version(components[0], components[1], components[2]);
  }

  Version(int major, int minor, int patch)
    : major_(major), minor_(minor), patch_(patch) {}

  bool operator == (const Version &other) const
  {
    return major_ == other.major_ &&
      minor_ == other.minor_ &&
      patch_ == other.patch_;
  }

  bool operator != (const Version &other) const
  {
    return !(*this == other);
  }

  bool operator < (const Version &other) const
  {
    // Lexicographic ordering.
    if (major_ != other.major_) {
      return major_ < other.major_;
    } else if (minor_ != other.minor_) {
      return minor_ < other.minor_;
    } else {
      return patch_ < other.patch_;
    }
  }

  bool operator > (const Version &other) const
  {
    // Lexicographic ordering.
    if (major_ != other.major_) {
      return major_ > other.major_;
    } else if (minor_ != other.minor_) {
      return minor_ > other.minor_;
    } else {
      return patch_ > other.patch_;
    }
  }

  bool operator <= (const Version &other) const
  {
    return *this < other || *this == other;
  }

  bool operator >= (const Version &other) const
  {
    return *this > other || *this == other;
  }

  friend inline std::ostream& operator << (std::ostream& s, const Version& v);

private:
  const int major_;
  const int minor_;
  const int patch_;
};


inline std::ostream& operator << (std::ostream& s, const Version& v)
{
  return s << v.major_ << "." << v.minor_ << "." << v.patch_;
}

#endif // __STOUT_VERSION_HPP__
