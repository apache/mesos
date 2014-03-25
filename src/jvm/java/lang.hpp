#ifndef __JAVA_LANG_HPP__
#define __JAVA_LANG_HPP__

#include <jvm/jvm.hpp>

namespace java {
namespace lang {

class Throwable : public Jvm::Object
{
public:
  explicit Throwable(const std::string& message)
  {
    static Jvm::Constructor constructor = Jvm::get()->findConstructor(
        Jvm::Class::named("java/lang/Throwable")
        .constructor()
        .parameter(Jvm::get()->stringClass));

    object = Jvm::get()->invoke(constructor, Jvm::get()->string(message));
  }

private:
  friend void Jvm::check(JNIEnv* env); // For constructing default instances.

  Throwable() {}
};

} // namespace lang {
} // namespace java {

#endif // __JAVA_LANG_HPP__
