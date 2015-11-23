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

#ifndef __ORG_APACHE_LOG4J_HPP__
#define __ORG_APACHE_LOG4J_HPP__

#include <jvm/jvm.hpp>

namespace org {
namespace apache {
namespace log4j {

// Forward declaration.
extern const char LEVEL_OFF_SIGNATURE[];
extern const char LEVEL_OFF[];


class Level : public Jvm::Object // TODO(benh): Extends Priority.
{
public:
  friend class Jvm::StaticVariable<Level, LEVEL_OFF, LEVEL_OFF_SIGNATURE>;

  static Jvm::StaticVariable<Level, LEVEL_OFF, LEVEL_OFF_SIGNATURE> OFF;

  Level() {} // No default constuctors.
};


class Category : public Jvm::Object
{
public:
  void setLevel(const Level& level)
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named("org/apache/log4j/Category")
        .method("setLevel")
        .parameter(Jvm::Class::named("org/apache/log4j/Level"))
        .returns(Jvm::get()->voidClass));

    Jvm::get()->invoke<void>(object, method, (jobject) level);
  }

protected:
  Category() {} // No default constructors.
};


class Logger : public Category
{
public:
  static Logger getRootLogger()
  {
    static Jvm::Method method = Jvm::get()->findStaticMethod(
        Jvm::Class::named("org/apache/log4j/Logger")
        .method("getRootLogger")
        .returns(Jvm::Class::named("org/apache/log4j/Logger")));

    Logger logger;
    logger.object = Jvm::get()->invokeStatic<jobject>(method);

    return logger;
  }

protected:
  Logger() {} // No default constructors.
};


} // namespace log4j {
} // namespace apache {
} // namespace org {

#endif // __ORG_APACHE_LOG4J_HPP__
