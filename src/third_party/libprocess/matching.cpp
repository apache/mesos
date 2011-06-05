#include <iostream>
#include <string>

using std::string;

#include "tuple.hpp"

namespace process { namespace tuple {

template <MSGID ID>
void match(const ::boost::variant<
 	   typename field<0, ID>::type &,
	   const typename field<0, ID>::type &,
 	   const ::boost::tuples::detail::swallow_assign &> &v0)
{
  if (typename field<0, ID>::type *t =
      ::boost::get<typename field<0, ID>::type &>(&v0))
    std::cout << "not const" << std::endl;
  else if (const typename field<0, ID>::type *t =
	   ::boost::get<const typename field<0, ID>::type &>(&v0))
    std::cout << "const" << std::endl;
  else
    std::cout << "ignore" << std::endl;
}

template <MSGID ID>
void match(const ::boost::variant<
 	   typename field<0, ID>::type &,
	   const typename field<0, ID>::type &,
 	   const ::boost::tuples::detail::swallow_assign &> &v0,
	   const ::boost::variant<
 	   typename field<1, ID>::type &,
	   const typename field<1, ID>::type &,
 	   const ::boost::tuples::detail::swallow_assign &> &v1)
{
  std::cout << "0:";
  if (typename field<0, ID>::type *t =
      ::boost::get<typename field<0, ID>::type &>(&v0))
    std::cout << "not const" << std::endl;
  else if (const typename field<0, ID>::type *t =
	   ::boost::get<const typename field<0, ID>::type &>(&v0))
    std::cout << "const" << std::endl;
  else
    std::cout << "ignore" << std::endl;

  std::cout << "1:";
  if (typename field<1, ID>::type *t =
      ::boost::get<typename field<1, ID>::type &>(&v1))
    std::cout << "not const" << std::endl;
  else if (const typename field<1, ID>::type *t =
	   ::boost::get<const typename field<1, ID>::type &>(&v1))
    std::cout << "const" << std::endl;
  else
    std::cout << "ignore" << std::endl;
}


template <MSGID ID>
void match(const ::boost::variant<
 	   typename field<0, ID>::type &,
	   const typename field<0, ID>::type &,
 	   const ::boost::tuples::detail::swallow_assign &> &v0,
	   const ::boost::variant<
 	   typename field<1, ID>::type &,
	   const typename field<1, ID>::type &,
 	   const ::boost::tuples::detail::swallow_assign &> &v1,
	   const ::boost::variant<
 	   typename field<2, ID>::type &,
	   const typename field<2, ID>::type &,
 	   const ::boost::tuples::detail::swallow_assign &> &v2)
{
  std::cout << "0:";
  if (typename field<0, ID>::type *t =
      ::boost::get<typename field<0, ID>::type &>(&v0))
    std::cout << "not const" << std::endl;
  else if (const typename field<0, ID>::type *t =
	   ::boost::get<const typename field<0, ID>::type &>(&v0))
    std::cout << "const" << std::endl;
  else
    std::cout << "ignore" << std::endl;

  std::cout << "1:";
  if (typename field<1, ID>::type *t =
      ::boost::get<typename field<1, ID>::type &>(&v1))
    std::cout << "not const" << std::endl;
  else if (const typename field<1, ID>::type *t =
	   ::boost::get<const typename field<1, ID>::type &>(&v1))
    std::cout << "const" << std::endl;
  else
    std::cout << "ignore" << std::endl;

  std::cout << "2:";
  if (typename field<2, ID>::type *t =
      ::boost::get<typename field<2, ID>::type &>(&v2))
    std::cout << "not const" << std::endl;
  else if (const typename field<1, ID>::type *t =
	   ::boost::get<const typename field<2, ID>::type &>(&v2))
    std::cout << "const" << std::endl;
  else
    std::cout << "ignore" << std::endl;
}


}}

enum { MSG = PROCESS_MSGID };

namespace process { namespace tuple {

TUPLE(MSG, (int, string));

}}

using process::tuple::match;
using process::tuple::_;


int main(int argc, char **argv)
{
  match<MSG>(10, "hello world");
  int i;
  match<MSG>(i, "hello world");
  match<MSG>(_, _);
  const int j = i;
  match<MSG>(j, _);
  return 0;
}
