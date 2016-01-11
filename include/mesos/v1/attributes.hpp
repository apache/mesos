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

#ifndef __MESOS_V1_ATTRIBUTES_HPP__
#define __MESOS_V1_ATTRIBUTES_HPP__

#include <iterator>
#include <string>

#include <mesos/v1/mesos.hpp>

#include <stout/option.hpp>

namespace mesos {
namespace v1 {

std::ostream& operator<<(std::ostream& stream, const Attribute& attribute);


class Attributes
{
public:
  Attributes() {}

  /*implicit*/
  Attributes(const google::protobuf::RepeatedPtrField<Attribute>& _attributes)
  {
    attributes.MergeFrom(_attributes);
  }

  /*implicit*/
  Attributes(const Attributes& that)
  {
    attributes.MergeFrom(that.attributes);
  }

  Attributes& operator=(const Attributes& that)
  {
    if (this != &that) {
      attributes.Clear();
      attributes.MergeFrom(that.attributes);
    }

    return *this;
  }

  bool operator==(const Attributes& that) const;


  bool operator!=(const Attributes& that) const
  {
    return !(*this == that);
  }

  size_t size() const
  {
    return attributes.size();
  }

  // Using this operator makes it easy to copy an attributes object into
  // a protocol buffer field.
  operator const google::protobuf::RepeatedPtrField<Attribute>&() const
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

  const Option<Attribute> get(const Attribute& thatAttribute) const;

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

  static bool isValid(const Attribute& attribute);

private:
  google::protobuf::RepeatedPtrField<Attribute> attributes;
};

} // namespace v1 {
} // namespace mesos {

#endif // __MESOS_V1_ATTRIBUTES_HPP__
