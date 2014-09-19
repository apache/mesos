#ifndef __PROCESS_NODE_HPP__
#define __PROCESS_NODE_HPP__

#include <unistd.h>

#include <sstream>

namespace process {

// Represents a remote "node" (encapsulates IP address and port).
class Node
{
public:
  Node(uint32_t _ip = 0, uint16_t _port = 0) : ip(_ip), port(_port) {}

  bool operator < (const Node& that) const
  {
    if (ip == that.ip) {
      return port < that.port;
    } else {
      return ip < that.ip;
    }
  }

  std::ostream& operator << (std::ostream& stream) const
  {
    stream << ip << ":" << port;
    return stream;
  }

  uint32_t ip;
  uint16_t port;
};

} // namespace process {

#endif // __PROCESS_NODE_HPP__
