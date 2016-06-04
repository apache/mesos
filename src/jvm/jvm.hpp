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

#ifndef __JVM_HPP__
#define __JVM_HPP__

#include <jni.h>

#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/try.hpp>


// Some compilers give us warnings about 'dereferencing type-punned
// pointer will break strict-aliasing rules' when we cast our JNIEnv**
// to void**. We use this function to do the magic for us.
inline void** JNIENV_CAST(JNIEnv** env)
{
  return reinterpret_cast<void**>(env);
}


struct JNI
{
  enum Version
  {
    v_1_1 = JNI_VERSION_1_1,
    v_1_2 = JNI_VERSION_1_2,
    v_1_4 = JNI_VERSION_1_4,
    v_1_6 = JNI_VERSION_1_6
  };
};

// Facilitates embedding a JVM and calling into it.
class Jvm
{
public:
  // Forward declarations.
  class ConstructorFinder;
  class MethodFinder;
  class Constructor;
  class MethodSignature;
  class Method;

  // Starts a new embedded JVM with the given -D options. Each option
  // supplied should be of the standard form: '-Dproperty=value'.
  // Returns the singleton Jvm instance or an error if the JVM had
  // already been created. Note that you can only create one JVM
  // instance per process since destructing a JVM has issues, see:
  // http://bugs.sun.com/bugdatabase/view_bug.do?bug_id=4712793.  In
  // addition, most JVM's use signals and couldn't possibly play
  // nicely with one another.
  // TODO(benh): Add a 'create' which just takes an already
  // constructed JavaVM. This will be useful for when a JVM is calling
  // into native code versus native code embedding a JVM.
  // TODO(John Sirois): Consider elevating classpath as a top-level
  // JVM configuration parameter since it will likely always need to
  // be specified. Ditto for and non '-X' options.
  static Try<Jvm*> create(
      const std::vector<std::string>& options = std::vector<std::string>(),
      JNI::Version version = JNI::v_1_6,
      bool exceptions = false);

  // Returns true if the JVM has already been created.
  static bool created();

  // Returns the singleton JVM instance, creating it with no options
  // and a default version if necessary.
  static Jvm* get();

  // An opaque class descriptor that can be used to find constructors,
  // methods and fields.
  class Class
  {
  public:
    // A factory for new Java reference type class descriptors given
    // the fully qualified class name (e.g., 'java/io/File'). To
    // obtain class descriptors for native types (int, short, etc),
    // use the fields in Jvm.
    static const Class named(const std::string& name);

    Class(const Class& that);

    // Returns the class of an array of the current class.
    const Class arrayOf() const;

    // Creates a builder that can be used to locate a constructor of
    // this class with Jvm::findConstructor.
    ConstructorFinder constructor() const;

    // Creates a builder that can be used to locate an instance method
    // of this class with Jvm::findMethod.
    MethodFinder method(const std::string& name) const;

  private:
    friend class Jvm;

    Class(const std::string& name, bool native = true);

    std::string signature() const;

    std::string name;
    bool native;
  };


  // A builder that is used to specify a constructor by specifying its
  // parameter list with zero or more calls to
  // ConstructorFinder::parameter.
  class ConstructorFinder
  {
  public:
    // Adds a parameter to the constructor parameter list.
    ConstructorFinder& parameter(const Class& clazz);

  private:
    friend class Class;
    friend class Jvm;

    explicit ConstructorFinder(const Class& clazz);

    const Class clazz;
    std::vector<Class> parameters;
  };


  // An opaque constructor descriptor that can be used to create new
  // instances of a class using Jvm::invokeConstructor.
  class Constructor
  {
  public:
    Constructor(const Constructor& that);

  private:
    friend class Jvm;

    Constructor(const Class& clazz, const jmethodID id);

    const Class clazz;
    const jmethodID id;
  };


  // A builder that is used to specify an instance method by
  // specifying its parameter list with zero or more calls to
  // MethodFinder::parameter and a final call to MethodFinder::returns
  // to get an opaque specification of the method for use with
  // Jvm::findMethod.
  class MethodFinder
  {
  public:
    // Adds a parameter to the method parameter list.
    MethodFinder& parameter(const Class& type);

    // Terminates description of a method by specifying its return type.
    MethodSignature returns(const Class& type) const;

  private:
    friend class Class;

    MethodFinder(const Class& clazz, const std::string& name);

    const Class clazz;
    const std::string name;
    std::vector<Class> parameters;
  };


  // An opaque method specification for use with Jvm::findMethod.
  class MethodSignature
  {
  public:
    MethodSignature(const MethodSignature& that);

  private:
    friend class Jvm;
    friend class MethodFinder;

    MethodSignature(const Class& clazz,
                    const std::string& name,
                    const Class& returnType,
                    const std::vector<Class>& parameters);

    const Class clazz;
    const std::string name;
    const Class returnType;
    std::vector<Class> parameters;
  };


  // An opaque method descriptor that can be used to invoke instance methods
  // using Jvm::invokeMethod.
  class Method
  {
  public:
    Method(const Method& that);

  private:
    friend class Jvm;
    friend class MethodSignature;

    Method(const Class& clazz, const jmethodID id);

    const Class clazz;
    const jmethodID id;
  };


  // An opaque field descriptor that can be used to access fields using
  // methods like Jvm::getStaticField.
  class Field
  {
  public:
    Field(const Field& that);

  private:
    friend class Jvm;

    Field(const Class& clazz, const jfieldID id);

    const Class clazz;
    const jfieldID id;
  };


  // Base class for all JVM objects. This object "stores" the
  // underlying global reference and performs the appropriate
  // operations across copies and assignments.
  class Object
  {
  public:
    Object() : object(nullptr) {}

    explicit Object(jobject _object)
      : object(Jvm::get()->newGlobalRef(_object)) {}

    Object(const Object& that)
      : object(nullptr)
    {
      if (that.object != nullptr) {
        object = Jvm::get()->newGlobalRef(that.object);
      }
    }

    ~Object()
    {
      if (object != nullptr) {
        Jvm::get()->deleteGlobalRef(object);
      }
    }

    Object& operator=(const Object& that)
    {
      if (object != nullptr) {
        Jvm::get()->deleteGlobalRef(object);
        object = nullptr;
      }
      if (that.object != nullptr) {
        object = Jvm::get()->newGlobalRef(that.object);
      }
      return *this;
    }

    operator jobject() const
    {
      return object;
    }

  protected:
    friend class Jvm; // For manipulating object.

    jobject object;
  };


  class Null : public Object {};


  template <typename T, const char* name, const char* signature>
  class Variable
  {
  public:
    explicit Variable(const Class& _clazz)
      : clazz(_clazz)
    {
      // Check that T extends Object.
      { T* t = nullptr; Object* o = t; (void) o; }
    }

    Variable(const Class& _clazz, const Object& _object)
      : clazz(_clazz), object(_object)
    {
      // Check that T extends Object.
      { T* t = nullptr; Object* o = t; (void) o; }
    }

    // TODO(benh): Implement cast operator (like in StaticVariable).
    // This requires implementing Jvm::getField too.

    template <typename U>
    Variable& operator=(const U& u)
    {
      // Check that U extends Object (but not necessarily T since U
      // might be 'Null').
      { U* u = nullptr; Object* o = u; (void) o; }

      // Note that we actually look up the field lazily (upon first
      // assignment operator) so that we don't possibly create the JVM
      // too early.
      static Field field = Jvm::get()->findField(clazz, name, signature);

      Jvm::get()->setField<jobject>(object, field, u);

      return *this;
    }

    void bind(const Object& _object) { object = _object; }

  private:
    const Class clazz;
    Object object; // Not const so we can do late binding.
  };

  // Helper for providing access to static variables in a class. You
  // can use this to delay the variable lookup until it's actually
  // accessed in order to keep the JVM from getting constructed too
  // early. See Level in jvm/org/apache/log4j.hpp for an example.
  // TODO(benh): Provide template specialization for primitive
  // types (e.g., StaticVariable<int>, StaticVariable<short>,
  // StaticVariable<std::string>).
  template <typename T, const char* name, const char* signature>
  class StaticVariable
  {
  public:
    explicit StaticVariable(const Class& _clazz)
      : clazz(_clazz)
    {
      // Check that T extends Object.
      { T* t = nullptr; Object* o = t; (void) o; }
    }

    operator T() const
    {
      // Note that we actually look up the field lazily (upon first
      // invocation operator) so that we don't possibly create the JVM
      // too early.
      static Field field =
        Jvm::get()->findStaticField(clazz, name, signature);
      T t;
      t.object = Jvm::get()->getStaticField<jobject>(field);
      return t;
    }

  private:
    const Class clazz;
  };

  // Each thread that wants to interact with the JVM needs a JNI
  // environment which must be obtained by "attaching" to the JVM. We
  // use the following RAII class to provide the environment and also
  // make sure a thread is attached and properly detached. Note that
  // nested constructions are no-ops and use the same environment (and
  // without detaching too early).
  // TODO(benh): Support putting a 'Jvm::Env' into a thread-local
  // variable so we can "attach" to the JVM once.
  class Env
  {
  public:
    explicit Env(bool daemon = true);
    ~Env();

    JNIEnv* operator->() const { return env; }

    operator JNIEnv*() const { return env; }

  private:
    JNIEnv* env;
    bool detach; // A nested use of Env should not detach the thread.
  };

  friend class Env;

  const Class voidClass;
  const Class booleanClass;
  const Class byteClass;
  const Class charClass;
  const Class shortClass;
  const Class intClass;
  const Class longClass;
  const Class floatClass;
  const Class doubleClass;
  const Class stringClass;

  jstring string(const std::string& s);

  Constructor findConstructor(const ConstructorFinder& finder);

  Method findMethod(const MethodSignature& signature);

  Method findStaticMethod(const MethodSignature& signature);

  Field findField(
      const Class& clazz,
      const std::string& name,
      const std::string& signature);

  Field findStaticField(
      const Class& clazz,
      const std::string& name,
      const std::string& signature);

  // TODO(John Sirois): Add "type checking" to variadic method
  // calls. Possibly a way to do this with typelists, type
  // concatenation and unwinding builder inheritance.

  jobject invoke(const Constructor ctor, ...);

  template <typename T>
  T invoke(const jobject receiver, const Method method, ...);

  template <typename T>
  T invokeStatic(const Method method, ...);

  template <typename T>
  void setField(jobject receiver, const Field& field, T t);

  template <typename T>
  T getStaticField(const Field& field);

  // Checks the exception state of an environment.
  void check(JNIEnv* env);

private:
  friend class Object; // For creating global references.

  Jvm(JavaVM* jvm, JNI::Version version, bool exceptions);
  ~Jvm();

private:
  jobject newGlobalRef(const jobject object);
  void deleteGlobalRef(const jobject object);

  jclass findClass(const Class& clazz);

  jmethodID findMethod(const Jvm::Class& clazz,
                       const std::string& name,
                       const Jvm::Class& returnType,
                       const std::vector<Jvm::Class>& argTypes,
                       bool isStatic);

  template <typename T>
  T invokeV(const jobject receiver, const jmethodID id, va_list args);

  template <typename T>
  T invokeStaticV(const Class& receiver, const jmethodID id, va_list args);

  // Singleton instance.
  static Jvm* instance;

  JavaVM* jvm;
  const JNI::Version version;
  const bool exceptions;
};


template <>
void Jvm::invoke<void>(const jobject receiver, const Method method, ...);


template <typename T>
T Jvm::invoke(const jobject receiver, const Method method, ...)
{
  va_list args;
  va_start(args, method);
  const T result = invokeV<T>(receiver, method.id, args);
  va_end(args);
  return result;
}


template <>
void Jvm::invokeStatic<void>(const Method method, ...);


template <typename T>
T Jvm::invokeStatic(const Method method, ...)
{
  va_list args;
  va_start(args, method);
  const T result = invokeStaticV<T>(method.clazz, method.id, args);
  va_end(args);
  return result;
}

#endif // __JVM_HPP__
