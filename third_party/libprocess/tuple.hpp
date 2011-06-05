#ifndef TUPLE_HPP
#define TUPLE_HPP

#include <process.hpp>
#include <serialization.hpp>

#include <sstream>
#include <string>
#include <utility>

#include <boost/tuple/tuple.hpp>

#include <boost/variant/variant.hpp>
#include <boost/variant/get.hpp>


/*
 * 
 * supermacro <tuple-impl.hpp>
 * mixins
 * boost tuples
 * serialization
 *
 */

/* Eventually we will want to support adding */
// const ::boost::tuples::detail::swallow_assign _ = ::boost::tuples::ignore;


#define IDENTITY(...) __VA_ARGS__

#define TUPLE(ID, types)                           \
template <> struct tuple<ID>                       \
{                                                  \
  typedef ::boost::tuple<IDENTITY types> type;     \
  mutable type t;                                  \
  tuple(const type &_t) : t(_t) {}                 \
}


namespace process { namespace serialization {

void operator & (serializer &, const ::boost::tuples::null_type &);

}}


#endif /* TUPLE_HPP */

