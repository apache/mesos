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

#ifndef __MESOS_V1_VALUES_HPP__
#define __MESOS_V1_VALUES_HPP__

#include <iosfwd>

#include <mesos/v1/mesos.hpp>

#include <stout/try.hpp>

namespace mesos {
namespace v1 {

std::ostream& operator<<(std::ostream& stream, const Value::Scalar& scalar);
bool operator==(const Value::Scalar& left, const Value::Scalar& right);
bool operator<(const Value::Scalar& left, const Value::Scalar& right);
bool operator<=(const Value::Scalar& left, const Value::Scalar& right);
bool operator>(const Value::Scalar& left, const Value::Scalar& right);
bool operator>=(const Value::Scalar& left, const Value::Scalar& right);
Value::Scalar operator+(const Value::Scalar& left, const Value::Scalar& right);
Value::Scalar operator-(const Value::Scalar& left, const Value::Scalar& right);
Value::Scalar& operator+=(Value::Scalar& left, const Value::Scalar& right);
Value::Scalar& operator-=(Value::Scalar& left, const Value::Scalar& right);

std::ostream& operator<<(std::ostream& stream, const Value::Ranges& ranges);
bool operator==(const Value::Ranges& left, const Value::Ranges& right);
bool operator<=(const Value::Ranges& left, const Value::Ranges& right);
Value::Ranges operator+(const Value::Ranges& left, const Value::Ranges& right);
Value::Ranges operator-(const Value::Ranges& left, const Value::Ranges& right);
Value::Ranges& operator+=(Value::Ranges& left, const Value::Ranges& right);
Value::Ranges& operator-=(Value::Ranges& left, const Value::Ranges& right);

std::ostream& operator<<(std::ostream& stream, const Value::Set& set);
bool operator==(const Value::Set& left, const Value::Set& right);
bool operator<=(const Value::Set& left, const Value::Set& right);
Value::Set operator+(const Value::Set& left, const Value::Set& right);
Value::Set operator-(const Value::Set& left, const Value::Set& right);
Value::Set& operator+=(Value::Set& left, const Value::Set& right);
Value::Set& operator-=(Value::Set& left, const Value::Set& right);

std::ostream& operator<<(std::ostream& stream, const Value::Text& value);
bool operator==(const Value::Text& left, const Value::Text& right);

namespace internal {
namespace values {

Try<Value> parse(const std::string& text);

} // namespace values {
} // namespace internal {

} // namespace v1 {
} // namespace mesos {

#endif // __MESOS_V1_VALUES_HPP__
