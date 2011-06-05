#include <nexus_types.hpp>
#include <sstream>
#include <string>

namespace nexus { 

slave_id slaveID_CPP2C(SlaveID sid) {
  return sid.c_str();
}
    
SlaveID slaveID_C2CPP(slave_id sid) {
  return std::string(sid);
}
    
}
