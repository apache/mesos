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

#ifndef __FACTORY_HPP__
#define __FACTORY_HPP__

#include <map>
#include <string>

#include <pthread.h>

#include <stout/fatal.hpp>

// These two macros create a Factory class that constructs instances of
// subclasses of a type T, whose constructors take a parameter of type P,
// based on a registered name for each subclass. To use them:
//
// 1) In a header file, call DECLARE_FACTORY(T, P).
//
// 2) In a source file, call DEFINE_FACTORY(T, P) { block }, where the block
//    calls registerClass<C>("name") for each subclass C of T to register.
//
// 3) You can now call TFactory::instantiate("name", p) to create an instance
//    of the class registered with a given name (or NULL if none exists).
//
// Note: Having to register all the classes in one file isn't ideal, but it
// seems to be the most foolproof solution. If we also want to allow classes
// to be loaded through shared libraries, it would be possible to add an init
// function to the library that registers them and make the registerClass()
// Another method people use to register classes is to have a static object
// whose constructor adds the class to the factory, but this method doesn't
// work without special care when the class is in a static library since the
// unreferenced static object won't be included by the compiler.


#define DECLARE_FACTORY(T, P) \
  class T##Factory : public ::mesos::factory::Factory<T, P> { \
    T##Factory(); \
    static T##Factory *instance; \
    static void initialize(); \
  public: \
    static T* instantiate(const std::string& name, P p); \
  };


#define DEFINE_FACTORY(T, P) \
  T##Factory *T##Factory::instance = 0; \
  namespace { \
    static pthread_once_t T##Factory_initialized = PTHREAD_ONCE_INIT; \
  } \
  void T##Factory::initialize() { \
    T##Factory::instance = new T##Factory(); \
  } \
  T * T##Factory::instantiate(const std::string& name, P p) { \
    pthread_once(&T##Factory_initialized, T##Factory::initialize); \
    return instance->instantiate2(name, p); \
  } \
  T##Factory::T##Factory() /* user code block follows */



// Helper classes for the factory macros.

namespace mesos {
namespace factory {

template<typename T, typename P> class Creator {
public:
  virtual T * instantiate(P p) = 0;

  virtual ~Creator() {}
};


template<typename C, typename T, typename P>
class ConcreteCreator : public Creator<T, P> {
public:
  virtual T* instantiate(P p) { return new C(); }
};


template<typename T, typename P> class Factory {
  std::map<std::string, Creator<T, P> *> creators;

protected:
  template<typename C> void registerClass(const std::string& name) {
    if (creators.find(name) != creators.end())
      fatal("Two classes registered with name \"%s\"", name.c_str());
    creators[name] = new ConcreteCreator<C, T, P>();
  }

  // Note: This instantiate needs to have a different name than the
  // static one in order to prevent infinite recursion in that one.
  T * instantiate2(const std::string& name, P p) {
    if (creators.find(name) == creators.end())
      return NULL;
    else
      return creators[name]->instantiate(p);
  }
};

} // namespace factory {
} // namespace mesos {


#endif
