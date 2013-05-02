#include <jvm/org/apache/log4j.hpp>

namespace org {
namespace apache {
namespace log4j {

// Static storage and initialization.
const char LEVEL_OFF[] = "OFF";

Jvm::StaticVariable<Level, LEVEL_OFF> Level::OFF =
  Jvm::StaticVariable<Level, LEVEL_OFF>(
      Jvm::Class::named("org/apache/log4j/Level"));

} // namespace log4j {
} // namespace apache {
} // namespace org {
