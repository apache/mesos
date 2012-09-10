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

#include <iostream>
#include <vector>

#include <glog/logging.h>

#include <stout/foreach.hpp>
#include <stout/strings.hpp>

#include "common/attributes.hpp"

using std::ostream;
using std::string;
using std::vector;


namespace mesos {
namespace internal {

Attribute Attributes::parse(const std::string& name, const std::string& text)
{
  Attribute attribute;
  Try<Value> result = values::parse(text);

  if (result.isError()) {
    LOG(FATAL) << "Failed to parse attribute " << name
               << " text " << text
               << " error " << result.error();
  } else{
    Value value = result.get();
    attribute.set_name(name);

    if (value.type() == Value::RANGES) {
      attribute.set_type(Value::RANGES);
      attribute.mutable_ranges()->MergeFrom(value.ranges());
    } else if (value.type() == Value::TEXT) {
      attribute.set_type(Value::TEXT);
      attribute.mutable_text()->MergeFrom(value.text());
    } else if (value.type() == Value::SCALAR) {
      attribute.set_type(Value::SCALAR);
      attribute.mutable_scalar()->MergeFrom(value.scalar());
    } else {
      LOG(FATAL) << "Bad type for attribute " << name
                 << " text " << text
                 << " type " << value.type();
    }
  }

  return attribute;
}


Attributes Attributes::parse(const string& s)
{
  // Tokenize and parse the value of "attributes".
  Attributes attributes;

  vector<string> tokens = strings::tokenize(s, ";\n");

  for (size_t i = 0; i < tokens.size(); i++) {
    const vector<string>& pairs = strings::tokenize(tokens[i], ":");
    if (pairs.size() != 2) {
      LOG(FATAL) << "Bad value for attributes, missing ':' within " << pairs[0];
    }

    attributes.add(parse(pairs[0], pairs[1]));
  }

  return attributes;
}

} // namespace internal {
} // namespace mesos {
