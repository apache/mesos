#include <jvm/org/apache/log4j.hpp>

namespace org {
namespace apache {
namespace log4j {

// Static storage and initialization.
const char LEVEL_OFF_SIGNATURE[] = "Lorg/apache/log4j/Level;";
const char LEVEL_OFF[] = "OFF";

Jvm::StaticVariable<Level, LEVEL_OFF, LEVEL_OFF_SIGNATURE> Level::OFF =
  Jvm::StaticVariable<Level, LEVEL_OFF, LEVEL_OFF_SIGNATURE>(
      Jvm::Class::named("org/apache/log4j/Level"));

} // namespace log4j {
} // namespace apache {
} // namespace org {
