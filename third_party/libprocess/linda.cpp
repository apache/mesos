#include <iostream>
#include <string>

#include <boost/tuple/tuple.hpp>

#include <boost/variant/variant.hpp>
#include <boost/variant/get.hpp>

#include <boost/type_traits.hpp>

using std::string;

// template <typename T0>
// void out(T0 &)
// {
//   std::cout << "not const" << std::endl;
// }

// template <typename T0>
// void out(const T0 &)
// {
//   std::cout << "const" << std::endl;
// }


// template <typename T0>
// void out(const ::boost::tuples::detail::swallow_assign &)
// {
//   std::cout << "ignore" << std::endl;
// }


// template <typename T0>
// void __out(const ::boost::variant<
// 	   T0 &,
// 	   const T0 &,
// 	   const ::boost::tuples::detail::swallow_assign &> &v0)
// {
//   if (T0 *t0 = ::boost::get<T0 &>(&v0))
//     std::cout << "not const" << std::endl;
//   else if (const T0 *t0 = ::boost::get<const T0 &>(&v0))
//     std::cout << "const" << std::endl;
//   else
//     std::cout << "ignore" << std::endl;
// }

template <typename T0>
void out(T0 t0)
{
  bool actual = false;

  if (::boost::is_const<T0>::value) {
    std::cout << "const" << std::endl;
    actual = true;
  } else if(::boost::is_scalar<T0>::value && !::boost::is_pointer<T0>::value) {
    std::cout << "scalar && !pointer" << std::endl;
    actual = true;
  }

  if (actual)
    std::cout << "actual" << std::endl;
  else
    std::cout << "parameter" << std::endl;
}


// template <typename T0, typename T1>
// void out(const ::boost::variant<
// 	 T0 &,
// 	 const T0 &,
// 	 const ::boost::tuples::detail::swallow_assign &> &v0,
// 	 const ::boost::variant<
// 	 T1 &,
// 	 const T1 &,
// 	 const ::boost::tuples::detail::swallow_assign &> &v1)
// {
//   std::cout << "0:";
//   if (T0 *t = ::boost::get<T0 &>(&v0))
//     std::cout << "not const" << std::endl;
//   else if (const T0 *t = ::boost::get<const T0 &>(&v0))
//     std::cout << "const" << std::endl;
//   else
//     std::cout << "ignore" << std::endl;

//   std::cout << "1:";
//   if (T1 *t = ::boost::get<T1 &>(&v1))
//     std::cout << "not const" << std::endl;
//   else if (const T1 *t = ::boost::get<const T1 &>(&v1))
//     std::cout << "const" << std::endl;
//   else
//     std::cout << "ignore" << std::endl;
// }


// template <typename T0, typename T1, typename T2>
// void out(const ::boost::variant<
// 	 T0 &,
// 	 const T0 &,
// 	 const ::boost::tuples::detail::swallow_assign &> &v0,
// 	 const ::boost::variant<
// 	 T1 &,
// 	 const T1 &,
// 	 const ::boost::tuples::detail::swallow_assign &> &v1,
// 	 const ::boost::variant<
// 	 T2 &,
// 	 const T2 &,
// 	 const ::boost::tuples::detail::swallow_assign &> &v2)
// {
//   std::cout << "0:";
//   if (T0 *t = ::boost::get<T0 &>(&v0))
//     std::cout << "not const" << std::endl;
//   else if (const T0 *t = ::boost::get<const T0 &>(&v0))
//     std::cout << "const" << std::endl;
//   else
//     std::cout << "ignore" << std::endl;

//   std::cout << "1:";
//   if (T1 *t = ::boost::get<T1 &>(&v1))
//     std::cout << "not const" << std::endl;
//   else if (const T1 *t = ::boost::get<const T1 &>(&v1))
//     std::cout << "const" << std::endl;
//   else
//     std::cout << "ignore" << std::endl;

//   std::cout << "2:";
//   if (T2 *t = ::boost::get<T2 &>(&v2))
//     std::cout << "not const" << std::endl;
//   else if (const T1 *t = ::boost::get<const T2 &>(&v2))
//     std::cout << "const" << std::endl;
//   else
//     std::cout << "ignore" << std::endl;
// }


const ::boost::tuples::detail::swallow_assign _ = ::boost::tuples::ignore;


int main(int argc, char **argv)
{
  out(10);
  int i;
  out(&i);
//   match(i, "hello world");
//   match(_, _);
  const int j = i;
  out(j);
  out(_);
  return 0;
}
