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

#ifndef __TOKENIZE_HPP__
#define __TOKENIZE_HPP__

#include <map>
#include <stdexcept>
#include <string>
#include <vector>


namespace tokenize {

class TokenizeException : public std::runtime_error
{
public:
  TokenizeException(const std::string& what) : std::runtime_error(what) {}
};


/**
 * Utility function to tokenize a string based on some delimiters.
 */
std::vector<std::string> split(const std::string& s,
                               const std::string& delims);


std::map<std::string, std::vector<std::string> > pairs(const std::string& s,
                                                       char delim1, char delim2);

} // namespace tokenize {

#endif // __TOKENIZE_HPP__





