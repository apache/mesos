#ifndef __NEXUS_TYPES_HPP__
#define __NEXUS_TYPES_HPP__

#include "nexus.h"
#include <cstdlib>
#include <sstream>
#include <string>

namespace nexus {

typedef std::string FrameworkID; // Unique within master
typedef std::string TaskID;      // Unique within framework
typedef std::string SlaveID;     // Unique within master
typedef std::string OfferID;     // Unique within master
typedef task_state TaskState;

}

#endif /* __NEXUS_TYPES_HPP__ */
