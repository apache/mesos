#include <jni.h>
#include <stdarg.h>

#include <glog/logging.h>

#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include <stout/hashmap.hpp>
#include <stout/foreach.hpp>

#include "jvm/jvm.hpp"


namespace mesos {
namespace internal {

jmethodID Jvm::findMethod(const Jvm::JClass& clazz,
                          const std::string& name,
                          const Jvm::JClass& returnType,
                          const std::vector<Jvm::JClass> argTypes,
                          bool isStatic)
{
  std::ostringstream signature;
  signature << "(";
  std::vector<Jvm::JClass>::iterator args;
  foreach (Jvm::JClass type, argTypes) {
    signature << type.signature();
  }
  signature << ")" << returnType.signature();

  LOG(INFO) << "looking up" << (isStatic ? " static " : " ") << "method "
            << name << signature.str();

  jmethodID id = NULL;
  if (isStatic) {
    id = env->GetStaticMethodID(findClass(clazz),
                                name.c_str(),
                                signature.str().c_str());
  } else {
    id = env->GetMethodID(findClass(clazz),
                          name.c_str(),
                          signature.str().c_str());
  }

  // TODO(John Sirois): consider CHECK -> return Option if re-purposing this
  // code outside of tests.
  CHECK(id != NULL);
  return id;
}


Jvm::ConstructorFinder::ConstructorFinder(
    const Jvm::JClass& _type)
    : type(_type),
      parameters() {}


Jvm::ConstructorFinder&
Jvm::ConstructorFinder::parameter(const Jvm::JClass& type)
{
  parameters.push_back(type);
  return *this;
}


Jvm::JConstructor Jvm::findConstructor(const ConstructorFinder& signature)
{
  jmethodID id =
      findMethod(signature.type,
                 "<init>",
                 voidClass,
                 signature.parameters,
                 false);
  return Jvm::JConstructor(signature.type, id);
}


Jvm::JConstructor::JConstructor(const JConstructor& other) : clazz(other.clazz),
                                                             id(other.id) {}


Jvm::JConstructor::JConstructor(const JClass& _clazz,
                                const jmethodID _id) : clazz(_clazz),
                                                       id(_id) {}


jobject Jvm::invoke(const JConstructor& ctor, ...)
{
  va_list args;
  va_start(args, ctor);
  jobject result = env->NewObjectV(findClass(ctor.clazz), ctor.id, args);
  va_end(args);
  CHECK(result != NULL);
  return result;
}


Jvm::MethodFinder::MethodFinder(
    const Jvm::JClass& _clazz,
    const std::string& _name)
    : clazz(_clazz),
      name(_name),
      parameters() {}


Jvm::MethodFinder& Jvm::MethodFinder::parameter(const JClass& type)
{
  parameters.push_back(type);
  return *this;
}


Jvm::MethodSignature Jvm::MethodFinder::returns(const JClass& returnType) const
{
  return Jvm::MethodSignature(clazz, name, returnType, parameters);
}


Jvm::JMethod Jvm::findMethod(const MethodSignature& signature)
{
  jmethodID id = findMethod(signature.clazz,
                            signature.name,
                            signature.returnType,
                            signature.parameters,
                            false);
  return Jvm::JMethod(signature.clazz, id);
}


Jvm::JMethod Jvm::findStaticMethod(const MethodSignature& signature)
{
  jmethodID id = findMethod(signature.clazz,
                            signature.name,
                            signature.returnType,
                            signature.parameters,
                            true);
  return Jvm::JMethod(signature.clazz, id);
}


Jvm::MethodSignature::MethodSignature(const MethodSignature& other) :
    clazz(other.clazz),
    name(other.name),
    returnType(other.returnType),
    parameters(other.parameters) {}


Jvm::MethodSignature::MethodSignature(const JClass& _clazz,
                                      const std::string& _name,
                                      const JClass& _returnType,
                                      const std::vector<JClass>& _parameters) :
    clazz(_clazz),
    name(_name),
    returnType(_returnType),
    parameters(_parameters) {}


Jvm::JMethod::JMethod(const JMethod& other)
    : clazz(other.clazz), id(other.id) {}


Jvm::JMethod::JMethod(const JClass& _clazz, const jmethodID _id)
    : clazz(_clazz), id(_id) {}


template <>
jobject Jvm::invokeV<jobject>(const jobject receiver,
                              const jmethodID id,
                              va_list args)
{
  jobject result = env->CallObjectMethodV(receiver, id, args);
  CHECK(result != NULL);
  return result;
}


template<>
void Jvm::invokeV<void>(const jobject receiver,
                        const jmethodID id,
                        va_list args)
{
  env->CallVoidMethodV(receiver, id, args);
}


template <>
bool Jvm::invokeV<bool>(const jobject receiver,
                        const jmethodID id,
                        va_list args)
{
  return env->CallBooleanMethodV(receiver, id, args);
}


template <>
char Jvm::invokeV<char>(const jobject receiver,
                        const jmethodID id,
                        va_list args)
{
  return env->CallCharMethodV(receiver, id, args);
}


template <>
short Jvm::invokeV<short>(const jobject receiver,
                          const jmethodID id,
                          va_list args)
{
  return env->CallShortMethodV(receiver, id, args);
}


template <>
int Jvm::invokeV<int>(const jobject receiver,
                      const jmethodID id,
                      va_list args)
{
  return env->CallIntMethodV(receiver, id, args);
}


template <>
long Jvm::invokeV<long>(const jobject receiver,
                        const jmethodID id,
                        va_list args)
{
  return env->CallLongMethodV(receiver, id, args);
}


template <>
float Jvm::invokeV<float>(const jobject receiver,
                          const jmethodID id,
                          va_list args)
{
  return env->CallFloatMethodV(receiver, id, args);
}


template <>
double Jvm::invokeV<double>(const jobject receiver,
                            const jmethodID id,
                            va_list args)
{
  return env->CallDoubleMethodV(receiver, id, args);
}


template <>
void Jvm::invoke<void>(const jobject receiver, const JMethod& method, ...)
{
  va_list args;
  va_start(args, method);
  invokeV<void>(receiver, method.id, args);
  va_end(args);
}


template <>
jobject Jvm::invokeStaticV<jobject>(const JClass& receiver,
                                    const jmethodID id,
                                    va_list args)
{
  jobject result = env->CallStaticObjectMethodV(findClass(receiver), id, args);
  CHECK(result != NULL);
  return result;
}


template<>
void Jvm::invokeStaticV<void>(const JClass& receiver,
                              const jmethodID id,
                              va_list args)
{
  env->CallStaticVoidMethodV(findClass(receiver), id, args);
}


template <>
bool Jvm::invokeStaticV<bool>(const JClass& receiver,
                              const jmethodID id,
                              va_list args)
{
  return env->CallStaticBooleanMethodV(findClass(receiver), id, args);
}


template <>
char Jvm::invokeStaticV<char>(const JClass& receiver,
                              const jmethodID id,
                              va_list args)
{
  return env->CallStaticCharMethodV(findClass(receiver), id, args);
}


template <>
short Jvm::invokeStaticV<short>(const JClass& receiver,
                                const jmethodID id,
                                va_list args)
{
  return env->CallStaticShortMethodV(findClass(receiver), id, args);
}


template <>
int Jvm::invokeStaticV<int>(const JClass& receiver,
                            const jmethodID id,
                            va_list args)
{
  return env->CallStaticIntMethodV(findClass(receiver), id, args);
}


template <>
long Jvm::invokeStaticV<long>(const JClass& receiver,
                              const jmethodID id,
                              va_list args)
{
  return env->CallStaticLongMethodV(findClass(receiver), id, args);
}


template <>
float Jvm::invokeStaticV<float>(const JClass& receiver,
                                const jmethodID id,
                                va_list args)
{
  return env->CallStaticFloatMethodV(findClass(receiver), id, args);
}


template <>
double Jvm::invokeStaticV<double>(const JClass& receiver,
                                  const jmethodID id,
                                  va_list args)
{
  return env->CallStaticDoubleMethodV(findClass(receiver), id, args);
}


template <>
void Jvm::invokeStatic<void>(const JMethod& method, ...)
{
  va_list args;
  va_start(args, method);
  invokeStaticV<void>(method.clazz, method.id, args);
  va_end(args);
}


const Jvm::JClass Jvm::JClass::forName(const std::string& nativeName)
{
  return Jvm::JClass(nativeName, false /* not a native type */);
}


Jvm::JClass::JClass(const JClass& other) : nativeName(other.nativeName),
                                           isNative(other.isNative) {}


Jvm::JClass::JClass(const std::string& _nativeName,
                    bool _isNative) : nativeName(_nativeName),
                                      isNative(_isNative) {}


const Jvm::JClass Jvm::JClass::arrayOf() const
{
  return Jvm::JClass("[" + nativeName, isNative);
}


Jvm::ConstructorFinder Jvm::JClass::constructor() const
{
  return Jvm::ConstructorFinder(*this);
}


Jvm::MethodFinder Jvm::JClass::method(const std::string& name) const
{
  return Jvm::MethodFinder(*this, name);
}


std::string Jvm::JClass::signature() const
{
  return isNative ? nativeName : "L" + nativeName + ";";
}


Jvm::JField::JField(const JField& other) : clazz(other.clazz), id(other.id) {}


Jvm::JField::JField(const JClass& _clazz, const jfieldID _id)
    : clazz(_clazz), id(_id) {}


Jvm::JField Jvm::findStaticField(const JClass& clazz, const std::string& name)
{
  jfieldID id =
      env->GetStaticFieldID(findClass(clazz),
                            name.c_str(),
                            clazz.signature().c_str());
  return Jvm::JField(clazz, id);
}


template <>
jobject Jvm::getStaticField<jobject>(const JField& field)
{
  jobject result = env->GetStaticObjectField(findClass(field.clazz), field.id);
  CHECK(result != NULL);
  return result;
}


template <>
bool Jvm::getStaticField<bool>(const JField& field)
{
  return env->GetStaticBooleanField(findClass(field.clazz), field.id);
}


template <>
char Jvm::getStaticField<char>(const JField& field)
{
  return env->GetStaticCharField(findClass(field.clazz), field.id);
}


template <>
short Jvm::getStaticField<short>(const JField& field)
{
  return env->GetStaticShortField(findClass(field.clazz), field.id);
}


template <>
int Jvm::getStaticField<int>(const JField& field)
{
  return env->GetStaticIntField(findClass(field.clazz), field.id);
}


template <>
long Jvm::getStaticField<long>(const JField& field)
{
  return env->GetStaticLongField(findClass(field.clazz), field.id);
}


template <>
float Jvm::getStaticField<float>(const JField& field)
{
  return env->GetStaticFloatField(findClass(field.clazz), field.id);
}


template <>
double Jvm::getStaticField<double>(const JField& field)
{
  return env->GetStaticDoubleField(findClass(field.clazz), field.id);
}


Jvm::Jvm(const std::vector<std::string>& options, JNIVersion jniVersion)
  : jvm(NULL),
    env(NULL),
    voidClass("V"),
    booleanClass("Z"),
    byteClass("B"),
    charClass("C"),
    shortClass("S"),
    intClass("I"),
    longClass("J"),
    floatClass("F"),
    doubleClass("D"),
    stringClass(JClass::forName("java/lang/String"))
{
  JavaVMInitArgs vmArgs;
  vmArgs.version = jniVersion;
  vmArgs.ignoreUnrecognized = false;

  JavaVMOption* opts = new JavaVMOption[options.size()];
  for (int i = 0; i < options.size(); i++) {
    opts[i].optionString = const_cast<char*>(options[i].c_str());
  }
  vmArgs.nOptions = options.size();
  vmArgs.options = opts;

  int result = JNI_CreateJavaVM(&jvm, (void**) &env, &vmArgs);
  CHECK(result != JNI_ERR) << "Failed to create JVM!";

  delete[] opts;
}


Jvm::~Jvm()
{
  CHECK(0 == jvm->DestroyJavaVM()) << "Failed to destroy JVM";
}


void Jvm::attachDaemon()
{
  jvm->AttachCurrentThreadAsDaemon((void**) &env, NULL);
}


void Jvm::attach()
{
  jvm->AttachCurrentThread((void**) &env, NULL);
}


void Jvm::detach()
{
  jvm->DetachCurrentThread();
}


jclass Jvm::findClass(const JClass& clazz)
{
  jclass cls = env->FindClass(clazz.nativeName.c_str());
  // TODO(John Sirois): consider CHECK -> return Option if re-purposing this
  // code outside of tests.
  CHECK(cls != NULL);
  return cls;
}


jobject Jvm::string(const std::string& str)
{
  return env->NewStringUTF(str.c_str());
}


jobject Jvm::newGlobalRef(const jobject object)
{
  return env->NewGlobalRef(object);
}


void Jvm::deleteGlobalRef(const jobject object)
{
  env->DeleteGlobalRef(object);
}


void Jvm::deleteGlobalRefSafe(const jobject object)
{
  if (object != NULL) {
    deleteGlobalRef(object);
  }
}

Jvm::Attach::Attach(Jvm* jvm, bool daemon) : _jvm(jvm)
{
  if (daemon) {
    _jvm->attachDaemon();
  } else {
    _jvm->attach();
  }
}


Jvm::Attach::~Attach()
{
  // TODO(John Sirois): this detaches too early under nested use, attach by a
  // given thread should incr, this should decr and only detach on 0
  _jvm->detach();
}

} // namespace internal
} // namespace mesos
