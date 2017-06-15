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

#include <stdarg.h>

#include <glog/logging.h>

#include <algorithm>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include <stout/dynamiclibrary.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

#include "common/build.hpp"
#include "jvm/jvm.hpp"
#include "jvm/java/lang.hpp"

// Static storage and initialization.
Jvm* Jvm::instance = nullptr;

Try<Jvm*> Jvm::create(
    const std::vector<std::string>& _options,
    JNI::Version version,
    bool exceptions)
{
  // TODO(benh): Make this thread-safe.
  if (instance != nullptr) {
    return Error("Java Virtual Machine already created");
  }

  JavaVMInitArgs vmArgs;
  vmArgs.version = version;
  vmArgs.ignoreUnrecognized = false;

  std::vector<std::string> options = _options;

#ifdef __APPLE__
  // The Apple JNI implementation requires the AWT thread to not
  // be the main application thread. Enabling headless mode
  // circumvents the issue. Further details:
  // https://issues.apache.org/jira/browse/MESOS-524
  // http://www.oracle.com/technetwork/articles/javase/headless-136834.html
  if (std::find(options.begin(), options.end(), "-Djava.awt.headless=true")
      == options.end()) {
    options.push_back("-Djava.awt.headless=true");
  }
#endif

  JavaVM* jvm = nullptr;
  JNIEnv* env = nullptr;
  Option<std::string> libJvmPath = os::getenv("JAVA_JVM_LIBRARY");

  if (libJvmPath.isNone()) {
    libJvmPath = mesos::internal::build::JAVA_JVM_LIBRARY;
  }

  static DynamicLibrary* libJvm = new DynamicLibrary();
  Try<Nothing> openResult = libJvm->open(libJvmPath.get());

  if (openResult.isError()) {
    return Error(openResult.error());
  }

  Try<void*> symbol = libJvm->loadSymbol("JNI_CreateJavaVM");

  if (symbol.isError()) {
    libJvm->close();
    return Error(symbol.error());
  }

  std::vector<JavaVMOption> opts(options.size());
  for (size_t i = 0; i < options.size(); i++) {
    opts[i].optionString = const_cast<char*>(options[i].c_str());
  }

  vmArgs.nOptions = opts.size();
  if (!opts.empty()) {
    vmArgs.options = &opts[0];
  }

  // typedef function pointer to JNI.
  typedef jint (*fnptr_JNI_CreateJavaVM)(JavaVM**, void**, void*);

  fnptr_JNI_CreateJavaVM fn_JNI_CreateJavaVM =
    (fnptr_JNI_CreateJavaVM)symbol.get();

  int createResult = fn_JNI_CreateJavaVM(&jvm, JNIENV_CAST(&env), &vmArgs);

  if (createResult == JNI_ERR) {
    libJvm->close();
    return Error("Failed to create JVM!");
  }

  return instance = new Jvm(jvm, version, exceptions);
}


bool Jvm::created()
{
  return instance != nullptr;
}


Jvm* Jvm::get()
{
  if (instance == nullptr) {
    create();
  }
  return CHECK_NOTNULL(instance);
}


Jvm::ConstructorFinder::ConstructorFinder(const Jvm::Class& _clazz)
  : clazz(_clazz), parameters() {}


Jvm::ConstructorFinder& Jvm::ConstructorFinder::parameter(
    const Jvm::Class& clazz)
{
  parameters.push_back(clazz);
  return *this;
}


Jvm::Constructor::Constructor(const Constructor& that)
  : clazz(that.clazz), id(that.id) {}


Jvm::Constructor::Constructor(const Class& _clazz, const jmethodID _id)
  : clazz(_clazz), id(_id) {}


Jvm::MethodFinder::MethodFinder(
    const Jvm::Class& _clazz,
    const std::string& _name)
  : clazz(_clazz),
    name(_name),
    parameters() {}


Jvm::MethodFinder& Jvm::MethodFinder::parameter(const Class& type)
{
  parameters.push_back(type);
  return *this;
}


Jvm::MethodSignature Jvm::MethodFinder::returns(const Class& returnType) const
{
  return Jvm::MethodSignature(clazz, name, returnType, parameters);
}


Jvm::MethodSignature::MethodSignature(const MethodSignature& that)
  : clazz(that.clazz),
    name(that.name),
    returnType(that.returnType),
    parameters(that.parameters) {}


Jvm::MethodSignature::MethodSignature(
    const Class& _clazz,
    const std::string& _name,
    const Class& _returnType,
    const std::vector<Class>& _parameters)
  : clazz(_clazz),
    name(_name),
    returnType(_returnType),
    parameters(_parameters) {}


Jvm::Method::Method(const Method& that)
    : clazz(that.clazz), id(that.id) {}


Jvm::Method::Method(const Class& _clazz, const jmethodID _id)
    : clazz(_clazz), id(_id) {}


const Jvm::Class Jvm::Class::named(const std::string& name)
{
  return Jvm::Class(name, false /* NOT a native type. */);
}


Jvm::Class::Class(const Class& that)
  : name(that.name), native(that.native) {}


Jvm::Class::Class(const std::string& _name, bool _native)
  : name(_name), native(_native) {}


const Jvm::Class Jvm::Class::arrayOf() const
{
  return Jvm::Class("[" + name, native);
}


Jvm::ConstructorFinder Jvm::Class::constructor() const
{
  return Jvm::ConstructorFinder(*this);
}


Jvm::MethodFinder Jvm::Class::method(const std::string& name) const
{
  return Jvm::MethodFinder(*this, name);
}


std::string Jvm::Class::signature() const
{
  return native ? name : "L" + name + ";";
}


Jvm::Field::Field(const Field& that)
  : clazz(that.clazz), id(that.id) {}


Jvm::Field::Field(const Class& _clazz, const jfieldID _id)
    : clazz(_clazz), id(_id) {}


Jvm::Env::Env(bool daemon)
  : env(nullptr), detach(false)
{
  JavaVM* jvm = Jvm::get()->jvm;

  // First check if we are already attached.
  int result = jvm->GetEnv(JNIENV_CAST(&env), Jvm::get()->version);

  // If we're not attached, attach now.
  if (result == JNI_EDETACHED) {
    if (daemon) {
      jvm->AttachCurrentThreadAsDaemon(JNIENV_CAST(&env), nullptr);
    } else {
      jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);
    }
    detach = true;
  }
}


Jvm::Env::~Env()
{
  if (detach) {
    Jvm::get()->jvm->DetachCurrentThread();
  }
}


jstring Jvm::string(const std::string& s)
{
  Env env;
  return env->NewStringUTF(s.c_str());
}


Jvm::Constructor Jvm::findConstructor(const ConstructorFinder& finder)
{
  jmethodID id = findMethod(
      finder.clazz,
      "<init>",
      voidClass,
      finder.parameters,
      false);

  return Jvm::Constructor(finder.clazz, id);
}


Jvm::Method Jvm::findMethod(const MethodSignature& signature)
{
  jmethodID id = findMethod(
      signature.clazz,
      signature.name,
      signature.returnType,
      signature.parameters,
      false);

  return Jvm::Method(signature.clazz, id);
}


Jvm::Method Jvm::findStaticMethod(const MethodSignature& signature)
{
  jmethodID id = findMethod(
      signature.clazz,
      signature.name,
      signature.returnType,
      signature.parameters,
      true);

  return Jvm::Method(signature.clazz, id);
}


Jvm::Field Jvm::findField(
    const Class& clazz,
    const std::string& name,
    const std::string& signature)
{
  Env env;

  jfieldID id = env->GetFieldID(
      findClass(clazz),
      name.c_str(),
      signature.c_str());

  check(env);

  return Jvm::Field(clazz, id);
}


Jvm::Field Jvm::findStaticField(
    const Class& clazz,
    const std::string& name,
    const std::string& signature)
{
  Env env;

  jfieldID id = env->GetStaticFieldID(
      findClass(clazz),
      name.c_str(),
      signature.c_str());

  check(env);

  return Jvm::Field(clazz, id);
}


jobject Jvm::invoke(const Constructor ctor, ...)
{
  Env env;
  va_list args;
  va_start(args, ctor);
  jobject o = env->NewObjectV(findClass(ctor.clazz), ctor.id, args);
  va_end(args);
  check(env);
  return o;
}


template <>
void Jvm::setField<jobject>(jobject receiver, const Field& field, jobject o)
{
  Env env;
  env->SetObjectField(receiver, field.id, o);
  check(env);
}


template <>
void Jvm::setField<bool>(jobject receiver, const Field& field, bool b)
{
  Env env;
  env->SetBooleanField(receiver, field.id, b);
  check(env);
}


template <>
void Jvm::setField<char>(jobject receiver, const Field& field, char c)
{
  Env env;
  env->SetCharField(receiver, field.id, c);
  check(env);
}


template <>
void Jvm::setField<short>(jobject receiver, const Field& field, short s)
{
  Env env;
  env->SetShortField(receiver, field.id, s);
  check(env);
}


template <>
void Jvm::setField<int>(jobject receiver, const Field& field, int i)
{
  Env env;
  env->SetIntField(receiver, field.id, i);
  check(env);
}


template <>
void Jvm::setField<long>(jobject receiver, const Field& field, long l)
{
  Env env;
  env->SetLongField(receiver, field.id, l);
  check(env);
}


template <>
void Jvm::setField<float>(jobject receiver, const Field& field, float f)
{
  Env env;
  env->SetFloatField(receiver, field.id, f);
  check(env);
}


template <>
void Jvm::setField<double>(jobject receiver, const Field& field, double d)
{
  Env env;
  env->SetDoubleField(receiver, field.id, d);
  check(env);
}


template <>
jobject Jvm::getStaticField<jobject>(const Field& field)
{
  Env env;
  jobject o = env->GetStaticObjectField(findClass(field.clazz), field.id);
  check(env);
  return o;
}


template <>
bool Jvm::getStaticField<bool>(const Field& field)
{
  Env env;
  bool b = env->GetStaticBooleanField(findClass(field.clazz), field.id);
  check(env);
  return b;
}


template <>
char Jvm::getStaticField<char>(const Field& field)
{
  Env env;
  char c = env->GetStaticCharField(findClass(field.clazz), field.id);
  check(env);
  return c;
}


template <>
short Jvm::getStaticField<short>(const Field& field)
{
  Env env;
  short s = env->GetStaticShortField(findClass(field.clazz), field.id);
  check(env);
  return s;
}


template <>
int Jvm::getStaticField<int>(const Field& field)
{
  Env env;
  int i = env->GetStaticIntField(findClass(field.clazz), field.id);
  check(env);
  return i;
}


template <>
long Jvm::getStaticField<long>(const Field& field)
{
  Env env;
  long l = env->GetStaticLongField(findClass(field.clazz), field.id);
  check(env);
  return l;
}


template <>
float Jvm::getStaticField<float>(const Field& field)
{
  Env env;
  float f = env->GetStaticFloatField(findClass(field.clazz), field.id);
  check(env);
  return f;
}


template <>
double Jvm::getStaticField<double>(const Field& field)
{
  Env env;
  double d = env->GetStaticDoubleField(findClass(field.clazz), field.id);
  check(env);
  return d;
}


Jvm::Jvm(JavaVM* _jvm, JNI::Version _version, bool _exceptions)
  : voidClass("V"),
    booleanClass("Z"),
    byteClass("B"),
    charClass("C"),
    shortClass("S"),
    intClass("I"),
    longClass("J"),
    floatClass("F"),
    doubleClass("D"),
    stringClass(Class::named("java/lang/String")),
    jvm(_jvm),
    version(_version),
    exceptions(_exceptions) {}


Jvm::~Jvm()
{
  LOG(FATAL) << "Destroying the JVM is not supported";
}


jobject Jvm::newGlobalRef(const jobject object)
{
  Env env;
  return env->NewGlobalRef(object);
}


void Jvm::deleteGlobalRef(const jobject object)
{
  Env env;
  if (object != nullptr) {
    env->DeleteGlobalRef(object);
  }
}


jclass Jvm::findClass(const Class& clazz)
{
  Env env;

  jclass jclazz = env->FindClass(clazz.name.c_str());

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    LOG(FATAL) << "Failed to find class " << clazz.name;
  }

  // TODO(John Sirois): Consider CHECK_NOTNULL -> return Option if
  // re-purposing this code outside of tests.
  return CHECK_NOTNULL(jclazz);
}


jmethodID Jvm::findMethod(
    const Jvm::Class& clazz,
    const std::string& name,
    const Jvm::Class& returnType,
    const std::vector<Jvm::Class>& argTypes,
    bool isStatic)
{
  Env env;

  std::ostringstream signature;
  signature << "(";
  foreach (const Jvm::Class& type, argTypes) {
    signature << type.signature();
  }
  signature << ")" << returnType.signature();

  LOG(INFO) << "Looking up" << (isStatic ? " static " : " ")
            << "method " << name << signature.str();

  jmethodID id = isStatic
    ? env->GetStaticMethodID(
        findClass(clazz),
        name.c_str(),
        signature.str().c_str())
    : env->GetMethodID(
        findClass(clazz),
        name.c_str(),
        signature.str().c_str());

  // TODO(John Sirois): Consider CHECK_NOTNULL -> return Option if
  // re-purposing this code outside of tests.
  return CHECK_NOTNULL(id);
}


template <>
void Jvm::invokeV<void>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  env->CallVoidMethodV(receiver, id, args);
  check(env);
}


template <>
jobject Jvm::invokeV<jobject>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  jobject o = env->CallObjectMethodV(receiver, id, args);
  check(env);
  return o;
}


template <>
bool Jvm::invokeV<bool>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  bool b = env->CallBooleanMethodV(receiver, id, args);
  check(env);
  return b;
}


template <>
char Jvm::invokeV<char>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  char c = env->CallCharMethodV(receiver, id, args);
  check(env);
  return c;
}


template <>
short Jvm::invokeV<short>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  short s = env->CallShortMethodV(receiver, id, args);
  check(env);
  return s;
}


template <>
int Jvm::invokeV<int>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  int i = env->CallIntMethodV(receiver, id, args);
  check(env);
  return i;
}


template <>
long Jvm::invokeV<long>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  long l = env->CallLongMethodV(receiver, id, args);
  check(env);
  return l;
}


template <>
float Jvm::invokeV<float>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  float f = env->CallFloatMethodV(receiver, id, args);
  check(env);
  return f;
}


template <>
double Jvm::invokeV<double>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  double d = env->CallDoubleMethodV(receiver, id, args);
  check(env);
  return d;
}


template <>
void Jvm::invokeStaticV<void>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  env->CallStaticVoidMethodV(findClass(receiver), id, args);
  check(env);
}


template <>
jobject Jvm::invokeStaticV<jobject>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  jobject o = env->CallStaticObjectMethodV(findClass(receiver), id, args);
  check(env);
  return o;
}


template <>
bool Jvm::invokeStaticV<bool>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  bool b = env->CallStaticBooleanMethodV(findClass(receiver), id, args);
  check(env);
  return b;
}


template <>
char Jvm::invokeStaticV<char>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  char c = env->CallStaticCharMethodV(findClass(receiver), id, args);
  check(env);
  return c;
}


template <>
short Jvm::invokeStaticV<short>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  short s = env->CallStaticShortMethodV(findClass(receiver), id, args);
  check(env);
  return s;
}


template <>
int Jvm::invokeStaticV<int>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  int i = env->CallStaticIntMethodV(findClass(receiver), id, args);
  check(env);
  return i;
}


template <>
long Jvm::invokeStaticV<long>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  long l = env->CallStaticLongMethodV(findClass(receiver), id, args);
  check(env);
  return l;
}


template <>
float Jvm::invokeStaticV<float>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  float f = env->CallStaticFloatMethodV(findClass(receiver), id, args);
  check(env);
  return f;
}


template <>
double Jvm::invokeStaticV<double>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  Env env;
  double d = env->CallStaticDoubleMethodV(findClass(receiver), id, args);
  check(env);
  return d;
}


void Jvm::check(JNIEnv* env)
{
  if (env->ExceptionCheck() == JNI_TRUE) {
    if (!exceptions) {
      env->ExceptionDescribe();
      EXIT(EXIT_FAILURE) << "Caught a JVM exception, not propagating";
    } else {
      java::lang::Throwable throwable;
      Object* object = &throwable;
      object->object = env->ExceptionOccurred();
      env->ExceptionClear();
      throw throwable;
    }
  }
}


// N.B. Both Jvm::invoke<void> and Jvm::invokeStatic<void> template
// instantiations need to be defined AFTER template instantions that
// they use (i.e., Jvm::invokeV<void>, Jvm::invokeStaticV<void>).

template <>
void Jvm::invoke<void>(const jobject receiver, const Method method, ...)
{
  va_list args;
  va_start(args, method);
  invokeV<void>(receiver, method.id, args);
  va_end(args);
}


template <>
void Jvm::invokeStatic<void>(const Method method, ...)
{
  va_list args;
  va_start(args, method);
  invokeStaticV<void>(method.clazz, method.id, args);
  va_end(args);
}
