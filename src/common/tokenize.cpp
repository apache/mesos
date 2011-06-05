#include <sstream>

#include "foreach.hpp"
#include "tokenize.hpp"

using std::map;
using std::string;
using std::ostringstream;
using std::vector;


namespace tokenize {

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


map<string, vector<string> > pairs(const string& s, const string& delim1, const string& delim2)
{
  map<string, vector<string> > result;

  const vector<string>& tokens = tokenize(s, delim1);
  foreach (const string& token, tokens) {
    const vector<string>& pairs = tokenize(token, delim2);
    if (pairs.size() != 2) {
      ostringstream out;
      out << "failed to split '" << token << "' with '" << delim2 << "'";
      throw TokenizeException(out.str());
    }

    result[pairs[0]].push_back(pairs[1]);
  }

  return result;
}

} // namespace tokenize {
