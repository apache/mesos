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

#ifndef __JAVA_IO_HPP__
#define __JAVA_IO_HPP__

#include <jvm/jvm.hpp>

namespace java {
namespace io {

class File : public Jvm::Object
{
public:
  explicit File(const std::string& pathname)
  {
    static Jvm::Constructor constructor = Jvm::get()->findConstructor(
        Jvm::Class::named("java/io/File")
        .constructor()
        .parameter(Jvm::get()->stringClass));

    object = Jvm::get()->invoke(constructor, Jvm::get()->string(pathname));
  }

  void deleteOnExit()
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named("java/io/File")
        .method("deleteOnExit")
        .returns(Jvm::get()->voidClass));

    Jvm::get()->invoke<void>(object, method);
  }
};

} // namespace io {
} // namespace java {

#endif // __JAVA_IO_HPP__
