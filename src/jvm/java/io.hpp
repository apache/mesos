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
