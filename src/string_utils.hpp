#ifndef __STRING_UTILS_HPP__
#define __STRING_UTILS_HPP__

#include <string>
#include <vector>

namespace nexus { namespace internal {

using std::string;
using std::vector;

/**
 * Contains utility functions for dealing with strings used throughout Mesos.
 */
class StringUtils
{
public:
  /**
   * Split a string around the character(s) in delims, placing the
   * resulting substrings into the tokens vector.
   */
  static void split(const string& str,
                    const string& delims,
                    vector<string>* tokens);

  /**
   * Trim a string, removing characters contained in toRemove from both
   * the beginning and end. If toRemove is not given, whitespace characters
   * are used by default.
   */
  static string trim(const string& str, const string& toRemove = " \t\n\r");
};

}} /* namespace nexus::internal */

#endif
