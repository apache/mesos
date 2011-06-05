#include "tokenize.hpp"

using std::string;
using std::vector;


namespace mesos { namespace internal {

vector<string> tokenize(const string& s, const string& delims)
{
  size_t offset = 0;
  vector<string> tokens;

  while (true) {
    size_t i = s.find_first_not_of(delims, offset);
    if (string::npos == i) {
      offset = s.length();
      return tokens;
    }

    size_t j = s.find_first_of(delims, i);
    if (string::npos == j) {
      tokens.push_back(s.substr(i));
      offset = s.length();
      continue;
    }

    tokens.push_back(s.substr(i, j - i));
    offset = j;
  }
}

}} /* namespace mesos { namespace internal { */
