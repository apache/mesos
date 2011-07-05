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

#include <sstream>

#include "foreach.hpp"
#include "tokenize.hpp"

using std::map;
using std::string;
using std::ostringstream;
using std::vector;


namespace tokenize {

vector<string> split(const string& s, const string& delims)
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


map<string, vector<string> > pairs(const string& s, char delim1, char delim2)
{
  map<string, vector<string> > result;

  const vector<string>& tokens = split(s, string(1, delim1));
  foreach (const string& token, tokens) {
    const vector<string>& pairs = split(token, string(1, delim2));
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
