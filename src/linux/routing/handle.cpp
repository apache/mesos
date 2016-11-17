// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ios>
#include <ostream>
#include <string>
#include <vector>

#include <stout/numify.hpp>
#include <stout/strings.hpp>

#include "handle.hpp"

using std::ostream;
using std::string;
using std::vector;

namespace routing {

Try<Handle> Handle::parse(const string& str)
{
  if (str == "root") {
    return EGRESS_ROOT;
  }

  vector<string> tokens = strings::tokenize(str, ":");
  if (tokens.size() != 2) {
    return Error("Failed to tokenize string: " + str);
  }

  // Handles should not have '0x' prefix, so we add '0x' to satisfy
  // numify() here. If they do have a '0x' prefix, we expect an error.
  Try<uint16_t> primary = numify<uint16_t>("0x" + tokens[0]);
  if (primary.isError()) {
    return Error("Failed to convert " + tokens[0] + " to a hex integer");
  }

  Try<uint16_t> secondary = numify<uint16_t>("0x" + tokens[1]);
  if (secondary.isError()) {
    return Error("Failed to convert " + tokens[1] + " to a hex integer");
  }

  return Handle(primary.get(), secondary.get());
}


ostream& operator<<(ostream& stream, const Handle& handle)
{
  stream << std::hex
         << handle.primary() << ":"
         << handle.secondary()
         << std::dec;
  return stream;
}

} // namespace routing {
