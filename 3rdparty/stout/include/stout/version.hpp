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

#ifndef __STOUT_VERSION_HPP__
#define __STOUT_VERSION_HPP__

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

// This class provides convenience routines for working with version
// numbers.  We support the SemVer 2.0.0 (http://semver.org) format,
// with two differences:
//
//   (1) Numeric components with leading zeros are allowed.
//
//   (2) Missing version components are allowed and treated as zero.
//
// TODO(neilc): Consider providing a "strict" variant that does not
// allow these extensions.
struct Version
{
  // Expect the string in the following format:
  //   <major>[.<minor>[.<patch>]][-prerelease][+build]
  //
  // Missing `minor` or `patch` components are treated as zero.
  //
  // `prerelease` is a prerelease label (e.g., "beta", "rc.1");
  // `build` is a build metadata label. Both `prerelease` and `build`
  // consist of one or more dot-separated identifiers. An identifier
  // is a non-empty string containing ASCII alphanumeric characters or
  // hyphens.
  //
  // Ideally, the version components would be called simply "major",
  // "minor", and "patch". However, GNU libstdc++ already defines
  // these as macros for compatibility reasons (man 3 makedev for more
  // information) implicitly included in every compilation.
  static Try<Version> parse(const std::string& input)
  {
    // The input string consists of the numeric components, optionally
    // followed by the prerelease label (prefixed with '-') and/or the
    // build label (prefixed with '+'). We parse the string from right
    // to left: build label (if any), prerelease label (if any), and
    // finally numeric components.

    std::vector<std::string> buildLabel;

    std::vector<std::string> buildParts = strings::split(input, "+", 2);
    CHECK(buildParts.size() == 1 || buildParts.size() == 2);

    if (buildParts.size() == 2) {
      const std::string& buildString = buildParts.back();

      Try<std::vector<std::string>> parsed = parseLabel(buildString);
      if (parsed.isError()) {
        return Error("Invalid build label: " + parsed.error());
      }

      buildLabel = parsed.get();
    }

    std::string remainder = buildParts.front();

    // Parse the prerelease label, if any. Note that the prerelease
    // label might itself contain hyphens.
    std::vector<std::string> prereleaseLabel;

    std::vector<std::string> prereleaseParts =
      strings::split(remainder, "-", 2);
    CHECK(prereleaseParts.size() == 1 || prereleaseParts.size() == 2);

    if (prereleaseParts.size() == 2) {
      const std::string& prereleaseString = prereleaseParts.back();

      Try<std::vector<std::string>> parsed = parseLabel(prereleaseString);
      if (parsed.isError()) {
        return Error("Invalid prerelease label: " + parsed.error());
      }

      prereleaseLabel = parsed.get();
    }

    remainder = prereleaseParts.front();

    constexpr size_t maxNumericComponents = 3;
    std::vector<std::string> numericComponents = strings::split(remainder, ".");

    if (numericComponents.size() > maxNumericComponents) {
      return Error("Version has " + stringify(numericComponents.size()) +
                   " components; maximum " + stringify(maxNumericComponents) +
                   " components allowed");
    }

    uint32_t versionNumbers[maxNumericComponents] = {0};

    for (size_t i = 0; i < numericComponents.size(); i++) {
      Try<uint32_t> result = parseNumericIdentifier(numericComponents[i]);
      if (result.isError()) {
        return Error("Invalid version component '" + numericComponents[i] + "'"
                     ": " + result.error());
      }

      versionNumbers[i] = result.get();
    }

    return Version(versionNumbers[0],
                   versionNumbers[1],
                   versionNumbers[2],
                   prereleaseLabel,
                   buildLabel);
  }

  // Construct a new Version. The `_prerelease` and `_build` arguments
  // contain lists of prerelease and build identifiers, respectively.
  Version(uint32_t _majorVersion,
          uint32_t _minorVersion,
          uint32_t _patchVersion,
          const std::vector<std::string>& _prerelease = {},
          const std::vector<std::string>& _build = {})
    : majorVersion(_majorVersion),
      minorVersion(_minorVersion),
      patchVersion(_patchVersion),
      prerelease(_prerelease),
      build(_build)
      {
        // As a sanity check, ensure that the caller has provided
        // valid prerelease and build identifiers.

        foreach (const std::string& identifier, prerelease) {
          CHECK_NONE(validateIdentifier(identifier));
        }

        foreach (const std::string& identifier, build) {
          CHECK_NONE(validateIdentifier(identifier));
        }
      }

  bool operator==(const Version& other) const
  {
    // NOTE: The `build` field is ignored when comparing two versions
    // for equality, per SemVer spec.
    return majorVersion == other.majorVersion &&
        minorVersion == other.minorVersion &&
        patchVersion == other.patchVersion &&
        prerelease == other.prerelease;
  }

  bool operator!=(const Version& other) const
  {
    return !(*this == other);
  }

  // SemVer 2.0.0 defines version precedence (ordering) like so:
  //
  //   Precedence MUST be calculated by separating the version into
  //   major, minor, patch and pre-release identifiers in that order
  //   (Build metadata does not figure into precedence). Precedence is
  //   determined by the first difference when comparing each of these
  //   identifiers from left to right as follows: Major, minor, and
  //   patch versions are always compared numerically. Example: 1.0.0
  //   < 2.0.0 < 2.1.0 < 2.1.1. When major, minor, and patch are
  //   equal, a pre-release version has lower precedence than a normal
  //   version. Example: 1.0.0-alpha < 1.0.0. Precedence for two
  //   pre-release versions with the same major, minor, and patch
  //   version MUST be determined by comparing each dot separated
  //   identifier from left to right until a difference is found as
  //   follows: identifiers consisting of only digits are compared
  //   numerically and identifiers with letters or hyphens are
  //   compared lexically in ASCII sort order. Numeric identifiers
  //   always have lower precedence than non-numeric identifiers. A
  //   larger set of pre-release fields has a higher precedence than a
  //   smaller set, if all of the preceding identifiers are equal.
  //   Example: 1.0.0-alpha < 1.0.0-alpha.1 < 1.0.0-alpha.beta <
  //   1.0.0-beta < 1.0.0-beta.2 < 1.0.0-beta.11 < 1.0.0-rc.1 < 1.0.0.
  //
  // NOTE: The `build` field is ignored when comparing two versions
  // for precedence, per the SemVer spec text above.
  bool operator<(const Version& other) const
  {
    // Compare version numbers numerically.
    if (majorVersion != other.majorVersion) {
      return majorVersion < other.majorVersion;
    }
    if (minorVersion != other.minorVersion) {
      return minorVersion < other.minorVersion;
    }
    if (patchVersion != other.patchVersion) {
      return patchVersion < other.patchVersion;
    }

    // If one version has a prerelease label and the other does not,
    // the prerelease version has lower precedence.
    if (!prerelease.empty() && other.prerelease.empty()) {
      return true;
    }
    if (prerelease.empty() && !other.prerelease.empty()) {
      return false;
    }

    // Compare two versions with prerelease labels by proceeding from
    // left to right.
    size_t minPrereleaseSize = std::min(
        prerelease.size(), other.prerelease.size());

    for (size_t i = 0; i < minPrereleaseSize; i++) {
      // Check whether the two prerelease identifiers can be converted
      // to numbers.
      Try<uint32_t> identifier = parseNumericIdentifier(prerelease.at(i));
      Try<uint32_t> otherIdentifier =
        parseNumericIdentifier(other.prerelease.at(i));

      if (identifier.isSome() && otherIdentifier.isSome()) {
        // Both identifiers are numeric.
        if (identifier.get() != otherIdentifier.get()) {
          return identifier.get() < otherIdentifier.get();
        }
      } else if (identifier.isSome()) {
        // `identifier` is numeric but `otherIdentifier` is not, so
        // `identifier` comes first.
        return true;
      } else if (otherIdentifier.isSome()) {
        // `otherIdentifier` is numeric but `identifier` is not, so
        // `otherIdentifier` comes first.
        return false;
      } else {
        // Neither identifier is numeric, so compare via ASCII sort.
        if (prerelease.at(i) != other.prerelease.at(i)) {
          return prerelease.at(i) < other.prerelease.at(i);
        }
      }
    }

    // If two versions have different numbers of prerelease labels but
    // they match on the common prefix, the version with the smaller
    // set of labels comes first.
    return prerelease.size() < other.prerelease.size();
  }

  bool operator>(const Version& other) const
  {
    return other < *this;
  }

  bool operator<=(const Version& other) const
  {
    return *this < other || *this == other;
  }

  bool operator>=(const Version& other) const
  {
    return *this > other || *this == other;
  }

  friend inline std::ostream& operator<<(
      std::ostream& stream,
      const Version& version);

  const uint32_t majorVersion;
  const uint32_t minorVersion;
  const uint32_t patchVersion;
  const std::vector<std::string> prerelease;
  const std::vector<std::string> build;

private:
  // Check that a string contains a valid identifier. An identifier is
  // a non-empty string; each character must be an ASCII alphanumeric
  // or hyphen. We allow leading zeros in numeric identifiers, which
  // inconsistent with the SemVer spec.
  static Option<Error> validateIdentifier(const std::string& identifier)
  {
    if (identifier.empty()) {
      return Error("Empty identifier");
    }

    auto alphaNumericOrHyphen = [](unsigned char c) -> bool {
      return std::isalnum(c) || c == '-';
    };

    auto firstInvalid = std::find_if_not(
        identifier.begin(), identifier.end(), alphaNumericOrHyphen);

    if (firstInvalid != identifier.end()) {
      return Error("Identifier contains illegal character: "
                   "'" + stringify(*firstInvalid) + "'");
    }

    return None();
  }

  // Parse a string containing a series of dot-separated identifiers
  // into a vector of strings; each element of the vector contains a
  // single identifier.
  static Try<std::vector<std::string>> parseLabel(const std::string& label)
  {
    if (label.empty()) {
      return Error("Empty label");
    }

    std::vector<std::string> identifiers = strings::split(label, ".");

    foreach (const std::string& identifier, identifiers) {
      Option<Error> error = validateIdentifier(identifier);
      if (error.isSome()) {
        return error.get();
      }
    }

    return identifiers;
  }

  // Try to parse the given string as a numeric identifier. According
  // to the SemVer spec, identifiers that begin with hyphens are
  // considered non-numeric.
  //
  // TODO(neilc): Consider adding a variant of `numify<T>` that only
  // supports non-negative inputs.
  static Try<uint32_t> parseNumericIdentifier(const std::string& identifier) {
    if (strings::startsWith(identifier, '-')) {
      return Error("Contains leading hyphen");
    }

    return numify<uint32_t>(identifier);
  }
};


inline std::ostream& operator<<(
    std::ostream& stream,
    const Version& version)
{
  stream << version.majorVersion << "."
         << version.minorVersion << "."
         << version.patchVersion;

  if (!version.prerelease.empty()) {
    stream << "-" << strings::join(".", version.prerelease);
  }

  if (!version.build.empty()) {
    stream << "+" << strings::join(".", version.build);
  }

  return stream;
}

#endif // __STOUT_VERSION_HPP__
