#ifndef __MESOS_TYPES_HPP__
#define __MESOS_TYPES_HPP__

#include "mesos.h"
#include <cstdlib>
#include <sstream>
#include <string>

namespace mesos {

typedef std::string FrameworkID; // Unique within master
typedef task_id TaskID;          // Unique within framework
typedef std::string SlaveID;     // Unique within master
typedef std::string OfferID;     // Unique within master
typedef task_state TaskState;

}

#endif /* __MESOS_TYPES_HPP__ */
