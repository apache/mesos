#include <iostream>
#include <string>

#include <tuple.hpp>

namespace process { namespace tuple {

enum Messages { REGISTER = PROCESS_MSGID, UNREGISTER, OKAY };

TUPLE(REGISTER, (std::string /*name*/));
TUPLE(UNREGISTER, (int /*id*/));
TUPLE(OKAY, (int /*response*/));

}}
