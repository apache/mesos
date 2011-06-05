#include "string_utils.hpp"

using std::string;
using std::vector;

using namespace nexus::internal;


void StringUtils::split(const string& str,
                        const string& delims,
                        vector<string>* tokens)
{
  // Start and end of current token; initialize these to the first token in
  // the string, skipping any leading delimiters
  size_t start = str.find_first_not_of(delims, 0);
  size_t end = str.find_first_of(delims, start);
  while (start != string::npos || end != string::npos) {
    // Add current token to the vector
    tokens->push_back(str.substr(start, end - start));
    // Advance start to first non-delimiter past the current end
    start = str.find_first_not_of(delims, end);
    // Advance end to the next delimiter after the new start
    end = str.find_first_of(delims, start);
  }
}


string StringUtils::trim(const string& str, const string& toRemove)
{
  // Start and end of current token; initialize these to the first token in
  // the string, skipping any leading delimiters
  size_t start = str.find_first_not_of(toRemove);
  size_t end = str.find_last_not_of(toRemove);
  if (start == string::npos) { // String contains only characters in toRemove
    return "";
  } else {
    return str.substr(start, end + 1 - start);
  }
}
