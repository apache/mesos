#ifndef __JAVA_NET_HPP__
#define __JAVA_NET_HPP__

#include <jvm/jvm.hpp>

namespace java {
namespace net {

class InetSocketAddress : public Jvm::Object // TODO(benh): Extends SocketAddress.
{
public:
  explicit InetSocketAddress(int port)
  {
    static Jvm::Constructor constructor = Jvm::get()->findConstructor(
        Jvm::Class::named("java/net/InetSocketAddress")
        .constructor()
        .parameter(Jvm::get()->intClass));

    object = Jvm::get()->invoke(constructor, port);
  }
};

} // namespace net {
} // namespace java {

#endif // __JAVA_NET_HPP__
