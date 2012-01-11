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

#include <boost/lexical_cast.hpp>

#include "common/foreach.hpp"
#include "common/resources.hpp"
#include "common/strings.hpp"
#include "common/values.hpp"

using std::ostream;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace values {

Try<Value> parse(const std::string& text) {
  Value value;

  // Remove any spaces from the text.
  string temp;
  foreach (const char c, text) {
    if (c != ' ') {
      temp += c;
    }
  }

  if (temp.length() == 0) {
    return Try<Value>::error(
      "Error parsing value, expecting non-empty string");
  }

  // TODO(ynie): Find a better way to check brackets.
  if (!strings::checkBracketsMatching(temp, '{', '}') ||
      !strings::checkBracketsMatching(temp, '[', ']') ||
      !strings::checkBracketsMatching(temp, '(', ')')) {
    return Try<Value>::error(
      "Error parsing value, brackets doesn't match");
  }

  size_t index = temp.find('[');
  if (index == 0) {
    // This is a ranges.
    Value::Ranges ranges;
    const vector<string>& tokens = strings::split(temp, "[]-,\n");
    if (tokens.size() % 2 != 0) {
      return Try<Value>::error("Error parsing value: " + text +
                               ", expect one or more \"ranges \"");
    } else {
      for (int i = 0; i < tokens.size(); i += 2) {
        Value::Range *range = ranges.add_range();

        int j = i;
        try {
          range->set_begin(boost::lexical_cast<uint64_t>((tokens[j++])));
          range->set_end(boost::lexical_cast<uint64_t>(tokens[j++]));
        } catch (const boost::bad_lexical_cast&) {
          return Try<Value>::error(
            "Error parsing value " + text +
            ", expecting non-negative integers in '" + tokens[j - 1] + "'");
        }
      }

      value.set_type(Value::RANGES);
      value.mutable_ranges()->MergeFrom(ranges);
      return Try<Value>::some(value);
    }
  } else if (index == string::npos) {
    size_t index = temp.find('{');
    if (index == 0) {
      // This is a set.
      Value::Set set;
      const vector<string>& tokens = strings::split(temp, "{},\n");
      for (int i = 0; i < tokens.size(); i++) {
        set.add_item(tokens[i]);
      }

      value.set_type(Value::SET);
      value.mutable_set()->MergeFrom(set);
      return Try<Value>::some(value);
    } else if (index == string::npos) {
      try {
        Value::Scalar scalar;
        scalar.set_value(boost::lexical_cast<double>(temp));
        // This is a Scalar.
        value.set_type(Value::SCALAR);
        value.mutable_scalar()->MergeFrom(scalar);
        return Try<Value>::some(value);
      } catch (const boost::bad_lexical_cast&) {
        // This is a Text.
        Value::Text text;
        text.set_value(temp);
        value.set_type(Value::TEXT);
        value.mutable_text()->MergeFrom(text);
        return Try<Value>::some(value);
      }
    } else {
      return Try<Value>::error(
        "Error parsing value " + text + ", bad '{' found");
    }
  } else {
    return Try<Value>::error(
      "Error parsing value " + text + ", bad '[' found");
  }

}

} // namespace values
} // namespace internal
} // namespace mesos
