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
