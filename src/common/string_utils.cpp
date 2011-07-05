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

#include "string_utils.hpp"

using std::string;
using std::vector;

using namespace mesos::internal;


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
