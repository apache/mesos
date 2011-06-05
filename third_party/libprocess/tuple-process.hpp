#ifndef TUPLE_PROCESS_HPP
#define TUPLE_PROCESS_HPP

#include <iostream>
#include <sstream>
#include <string>

#include <process.hpp>

#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>

#include <boost/tuple/tuple.hpp>

#include <boost/preprocessor/array.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/control.hpp>
#include <boost/preprocessor/facilities.hpp>
#include <boost/preprocessor/repetition.hpp>

#include "variadic.hpp"

using std::string;


/*
 * A Message is a struct templated by its MSGID. By using the MESSAGE
 * macro one can define their own message types as follows:
 *
 *   MESSAGE(MYMSG, int, string);
 *
 * Later, one can instantiate one of their messages by doing:
 *
 *   Message<MYMSG> m(42, "hello world");
 *
 * Most importantly, a message can be used anywhere a tuple is
 * expected (see the 'match' macro below).
 *
 * For now, this is declared inside the mesos internal namespace, but
 * in the future it would be nice to factor this out so other
 * libraries could use it for sending messages.
 */

template <MSGID ID> struct Message {};

#define GENERATE_REF_TYPE(z, index, types) \
  BOOST_PP_ARRAY_ELEM(index, types) &

#define GENERATE_REF_TYPE_LIST(types)                \
  BOOST_PP_ENUM(BOOST_PP_ARRAY_SIZE(types), GENERATE_REF_TYPE, types)

#define GENERATE_TYPE(z, index, types) \
  BOOST_PP_ARRAY_ELEM(index, types)

#define GENERATE_TYPE_LIST(types)                \
  BOOST_PP_ENUM(BOOST_PP_ARRAY_SIZE(types), GENERATE_TYPE, types)

#define GENERATE_ARG(z, index, _) \
  BOOST_PP_CAT(a, index)

#define GENERATE_ARG_LIST(types)                \
  BOOST_PP_ENUM(BOOST_PP_ARRAY_SIZE(types), GENERATE_ARG, ~)

#define GENERATE_PARAM(z, index, types) \
  BOOST_PP_ARRAY_ELEM(index, types) BOOST_PP_CAT(a, index)

#define GENERATE_PARAM_LIST(types)                \
  BOOST_PP_ENUM(BOOST_PP_ARRAY_SIZE(types), GENERATE_PARAM, types)

#define GENERATE_GET(z, index, _) \
  ::boost::get< index > (tuple)

#define GENERATE_GET_LIST(types)                \
  BOOST_PP_ENUM(BOOST_PP_ARRAY_SIZE(types), GENERATE_GET, ~)

#define GENERATE_OPERATOR_REF_TUPLE(types) \
  operator ::boost::tuple<GENERATE_REF_TYPE_LIST(types) > () \
  { \
    return ::boost::tuple<GENERATE_REF_TYPE_LIST(types) > \
      (GENERATE_GET_LIST(types)); \
  }

#define PAIR(f, a) f a

#define IDENTITY(...) __VA_ARGS__

#define MESSAGE_IMPL(ID, types)      \
template <> \
struct ::Message<ID> \
{ \
  ::boost::tuple<GENERATE_TYPE_LIST(types) > tuple;  \
\
  Message(GENERATE_PARAM_LIST(types)) \
  { \
    tuple = ::boost::make_tuple(GENERATE_ARG_LIST(types)); \
  } \
\
  Message(std::pair<const char *, size_t> body) \
  { \
    deserialize(string(body.first, body.second)); \
  } \
\
  operator ::boost::tuple<GENERATE_TYPE_LIST(types) > () \
  { \
    return tuple; \
  } \
\
  PAIR(IDENTITY, BOOST_PP_APPLY(BOOST_PP_IF(BOOST_PP_ARRAY_SIZE(types), ((GENERATE_OPERATOR_REF_TUPLE(types))), (())))) \
\
  operator ::boost::tuple< string &, const ::boost::tuples::detail::swallow_assign &, const ::boost::tuples::detail::swallow_assign & > () \
  { \
    return ::boost::tuple< string &, const ::boost::tuples::detail::swallow_assign &, const ::boost::tuples::detail::swallow_assign & > \
      (::boost::get< 0 > (tuple), ::boost::tuples::ignore, ::boost::tuples::ignore); \
  } \
\
  string serialize() const \
  { \
    return string(); \
  } \
\
  void deserialize(string s) \
  { \
  } \
}

/* TODO(benh): Make this variadic macro pedantic compliant! */
#define MESSAGE(ID, ...) \
  MESSAGE_IMPL(ID, BOOST_PP_VA_ARGS_TO_ARRAY(__VA_ARGS__))


/*
 * The match macro just adds a bit of syntactic sugar to make matching
 * messages seem cleaner. You can match a message that has a tuple
 * type of (int, string) by doing:
 *
 *    Message<ID> m(42, "hello world");
 *
 *    int i; string s;
 *    match (i, s) = m;
 *
 * It might be interesting to see how in the future one might be able
 * to have even prettier syntactic sugar:
 * 
 *   match m with (int i, string s) {}
 *
 * This isn't actually that difficult to accomplish, and will be even
 * easier to accomplish once 'auto' and 'decltype' get added to the
 * language. Note that the above syntax might also allow for better
 * optimized code because one could actually match a 'string' with a
 * 'string &', eschewing the extra copy.
 */
#define match(args...) ::boost::tie(args)

//::boost::tuples::detail::swallow_assign _ = boost::tuples::detail::swallow_assign();

//typeof(::boost::tuples::ignore) _ = ::boost::tuples::ignore;

#define _ ::boost::tuples::ignore


// /*
//  * The serialization we use for tuple messages is pretty simple, we
//  * actually abort on types we haven't implemented.
//  */

// class Serializer
// {
//   std::ostringstream& stream;

//   Serializer(std::ostringstream& s) : stream(s) {}

//   template<typename T>
//   void operator & (T& t)
//   {
//     abort();
//   }

//   template<>
//   void operator & (int32_t i)
//   {
//     uint32_t netInt = htonl((uint32_t) i);
//     stream.write(&netInt, sizeof(netInt));
//   }

//   template<>
//   void operator & (int64_t i)
//   {
//     uint32_t hiInt = htonl((uint32_t) (i >> 32));
//     uint32_t loInt = htonl((uint32_t) (i & 0xFFFFFFFF));
//     stream.write(&hiInt, sizeof(hiInt));
//     stream.write(&loInt, sizeof(loInt));
//   }

//   template<>
//   void operator & (const std::string& str)
//   {
//     *this & ((int32_t) str.size());
//     stream.write(str.data(), str.size());
//   }
// };


// class Deserializer
// {
//   std::istringstream& stream;

//   Deserializer(std::istringstream& s) : stream(s) {}

//   template<typename T>
//   void operator & (T& t)
//   {
//     abort();
//   }

//   template<>
//   void operator & (int32_t i)
//   {
//     uint32_t netInt;
//     stream.read(&netInt, sizeof(netInt));
//     return (int32_t) ntohl(netInt);
//   }

//   template<>
//   void operator & (int64_t i)
//   {
//     uint32_t hiInt, loInt;
//     stream.read(&hiInt, sizeof(hiInt));
//     stream.read(&loInt, sizeof(loInt));
//     int64_t hi64 = ntohl(hiInt);
//     int64_t lo64 = ntohl(loInt);
//     return (hi64 << 32) | lo64;
//   }

//   template<>
//   void operator & (std::string& str)
//   {
//     int32_t size;
//     *this & size;
//     str.resize((size_t) size);
//     for (int i = 0; i < size; i++)
//       str[i] = (char) stream.get();
//   }
// };



// namespace boost { namespace serialization {

// template <class Archive>
// void serialize(Archive &ar, ::boost::tuple<> &t, const unsigned int version) {}

// #define GENERATE_ELEMENT_SERIALIZE(z, which, unused)                     \
//     ar & t.get<which>();
    
// #define GENERATE_TUPLE_SERIALIZE(z, nargs, unused)                       \
//     template <class Archive, BOOST_PP_ENUM_PARAMS(nargs, class T)>       \
//     void serialize(Archive &ar,                                          \
//                    ::boost::tuple<BOOST_PP_ENUM_PARAMS(nargs, T)> &t,      \
//                    const unsigned int version)                           \
//     {                                                                    \
//       BOOST_PP_REPEAT_FROM_TO(0, nargs, GENERATE_ELEMENT_SERIALIZE, ~);  \
//     }

//   BOOST_PP_REPEAT_FROM_TO(1, 10, GENERATE_TUPLE_SERIALIZE, ~);
// }}

/*
 * A TupleProcess is a Process however with the send routines
 * overloaded with templates to allow for type-safe sends. In
 * addition, a method 'message' is added to allow for easy casting of
 * received message bodies.
 */

class TupleProcess : public Process
{
protected:
  template <MSGID ID>
  void send(const PID &to)
  {
    Message<ID> m();
    string data = m.serialize();
    //Process::send(to, ID, data.c_str(), data.size());
  }

#define GENERATE_SEND(z, index, _)                       \
    template <MSGID ID, BOOST_PP_ENUM_PARAMS(index, typename T)>       \
    void send(const PID &to,                                          \
                   BOOST_PP_ENUM_BINARY_PARAMS(index, const T, &t)) \
    {                                                                    \
      Message<ID> m(BOOST_PP_ENUM_PARAMS(index, t)); \
      string data = m.serialize(); \
    }

//    Process::send(to, ID, data.c_str(), data.size()); \
//  }

  BOOST_PP_REPEAT_FROM_TO(1, 10, GENERATE_SEND, ~);

  template <MSGID ID>
  Message<ID> message()
  {
    return Message<ID>(body());
  }
};


#endif /* MESSAGE_HPP */
