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

#ifndef __ATTRIBUTES_HPP__
#define __ATTRIBUTES_HPP__

#include <iterator>
#include <string>

#include <mesos/mesos.hpp>
#include <mesos/values.hpp>

#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

#include "logging/logging.hpp"

namespace mesos {


std::ostream& operator << (std::ostream& stream, const Attribute& attribute);


namespace internal {


class Attributes
{
public:
  Attributes() {}

  /*implicit*/
  Attributes(const google::protobuf::RepeatedPtrField<Attribute>& _attributes)
  {
    attributes.MergeFrom(_attributes);
  }

  Attributes(const Attributes& that)
  {
    attributes.MergeFrom(that.attributes);
  }

  Attributes& operator = (const Attributes& that)
  {
    if (this != &that) {
      attributes.Clear();
      attributes.MergeFrom(that.attributes);
    }

    return *this;
  }

  bool operator == (const Attributes& that) const
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

  bool operator != (const Attributes& that) const
  {
    return !(*this == that);
  }

  size_t size() const
  {
    return attributes.size();
  }

  // Using this operator makes it easy to copy a attributes object into
  // a protocol buffer field.
  operator const google::protobuf::RepeatedPtrField<Attribute>& () const
  {
    return attributes;
  }

  void add(const Attribute& attribute)
  {
    attributes.Add()->MergeFrom(attribute);
  }

  const Attribute get(int index) const
  {
    return attributes.Get(index);
  }

  const Option<Attribute> get(const Attribute& thatAttribute) const
  {
    foreach (const Attribute& attribute, attributes) {
      if (attribute.name() == thatAttribute.name() &&
          attribute.type() == thatAttribute.type()) {
        return attribute;
      }
    }

    return None();
  }

  template <typename T>
  T get(const std::string& name, const T& t) const;

  typedef google::protobuf::RepeatedPtrField<Attribute>::iterator
  iterator;

  typedef google::protobuf::RepeatedPtrField<Attribute>::const_iterator
  const_iterator;

  iterator begin() { return attributes.begin(); }
  iterator end() { return attributes.end(); }

  const_iterator begin() const { return attributes.begin(); }
  const_iterator end() const { return attributes.end(); }

  static Attribute parse(const std::string& name, const std::string& value);
  static Attributes parse(const std::string& s);

  static bool isValid(const Attribute& attribute)
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

private:
  google::protobuf::RepeatedPtrField<Attribute> attributes;
};


template <>
inline Value::Scalar Attributes::get(
    const std::string& name,
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
inline Value::Ranges Attributes::get(
    const std::string& name,
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
inline Value::Text Attributes::get(
    const std::string& name,
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


} // namespace internal {
} // namespace mesos {

#endif // __ATTRIBUTES_HPP__
