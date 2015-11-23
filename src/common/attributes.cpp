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

#include <iostream>
#include <vector>

#include <glog/logging.h>

#include <mesos/attributes.hpp>
#include <mesos/values.hpp>

#include <stout/foreach.hpp>
#include <stout/strings.hpp>

using std::ostream;
using std::string;
using std::vector;


namespace mesos {

std::ostream& operator<<(std::ostream& stream, const Attribute& attribute)
{
  stream << attribute.name() << "=";
  switch (attribute.type()) {
    case Value::SCALAR: stream << attribute.scalar(); break;
    case Value::RANGES: stream << attribute.ranges(); break;
    case Value::SET:    stream << attribute.set();    break;
    case Value::TEXT:   stream << attribute.text();   break;
    default:
      LOG(FATAL) << "Unexpected Value type: " << attribute.type();
      break;
  }

  return stream;
}


bool Attributes::operator==(const Attributes& that) const
{
  if (size() != that.size()) {
    return false;
  }

  foreach (const Attribute& attribute, attributes) {
    Option<Attribute> maybeAttribute = that.get(attribute);
    if (maybeAttribute.isNone()) {
        return false;
    }
    const Attribute& thatAttribute = maybeAttribute.get();
    switch (attribute.type()) {
    case Value::SCALAR:
      if (!(attribute.scalar() == thatAttribute.scalar())) {
        return false;
      }
      break;
    case Value::RANGES:
      if (!(attribute.ranges() == thatAttribute.ranges())) {
        return false;
      }
      break;
    case Value::TEXT:
      if (!(attribute.text() == thatAttribute.text())) {
        return false;
      }
      break;
    case Value::SET:
      LOG(FATAL) << "Sets not supported for attributes";
    }
  }

  return true;
}


const Option<Attribute> Attributes::get(const Attribute& thatAttribute) const
{
  foreach (const Attribute& attribute, attributes) {
    if (attribute.name() == thatAttribute.name() &&
        attribute.type() == thatAttribute.type()) {
      return attribute;
    }
  }

  return None();
}


Attribute Attributes::parse(const string& name, const string& text)
{
  Attribute attribute;
  Try<Value> result = internal::values::parse(text);

  if (result.isError()) {
    LOG(FATAL) << "Failed to parse attribute " << name
               << " text " << text
               << " error " << result.error();
  } else {
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
    const vector<string>& pairs = strings::split(tokens[i], ":", 2);
    if (pairs.size() != 2 || pairs[0].empty() || pairs[1].empty()) {
      LOG(FATAL) << "Invalid attribute key:value pair '" << tokens[i] << "'";
    }

    attributes.add(parse(pairs[0], pairs[1]));
  }

  return attributes;
}


bool Attributes::isValid(const Attribute& attribute)
{
  if (!attribute.has_name() ||
      attribute.name() == "" ||
      !attribute.has_type() ||
      !Value::Type_IsValid(attribute.type())) {
    return false;
    }

  if (attribute.type() == Value::SCALAR) {
    return attribute.has_scalar();
  } else if (attribute.type() == Value::RANGES) {
    return attribute.has_ranges();
  } else if (attribute.type() == Value::TEXT) {
    return attribute.has_text();
  } else if (attribute.type() == Value::SET) {
    // Attributes doesn't support set.
    return false;
  }

    return false;
}


template <>
Value::Scalar Attributes::get(
    const string& name,
    const Value::Scalar& scalar) const
{
  foreach (const Attribute& attribute, attributes) {
    if (attribute.name() == name &&
        attribute.type() == Value::SCALAR) {
      return attribute.scalar();
    }
  }

  return scalar;
}


template <>
Value::Ranges Attributes::get(
    const string& name,
    const Value::Ranges& ranges) const
{
  foreach (const Attribute& attribute, attributes) {
    if (attribute.name() == name &&
        attribute.type() == Value::RANGES) {
      return attribute.ranges();
    }
  }

  return ranges;
}


template <>
Value::Text Attributes::get(
    const string& name,
    const Value::Text& text) const
{
  foreach (const Attribute& attribute, attributes) {
    if (attribute.name() == name &&
        attribute.type() == Value::TEXT) {
      return attribute.text();
    }
  }

  return text;
}

} // namespace mesos {
