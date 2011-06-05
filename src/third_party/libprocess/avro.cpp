#include <iostream>

#include <boost/preprocessor.hpp>

/* AVRO_LONG */
/* IS_PRIMITIVE ## _TRUE */

/* IS_PRIMITIVE (LongList()) */

/* // if the record has one thing in it, make its type just that thing */
/* // if the record has only primitive types in it, make its type a tuple of the primitives */
/* // if the record has both primitive and complex types, make its type the actual struct type */

/* // have flags in the struct to determine if it is a tuple type */


/* send_primitive(); */
/* send_record(pid, pack<LongList>()); */
/* send_enum(pid, ); */
/* send_array(pid, ); */
/* send_map(pid, ); */
/* send_union(pid, ); */
/* send_fixed(pid, ); */

/* // something that works for simple records and something that won't compile for more complex records */

/* send<REGISTER>(pid, 42, 3.45, "hi"); */

/* send<REGISTER(pid, make_avro_record()); */


/* struct __LongList { */
/*   long value; */
/*   union { */
/*     struct __LongList *__next; */
/*     __avro_null __next; */
/*   } next; */

/*   typedef ::boost::tuple<long, ::boost::variant<> > type; */
/* }; */


/* struct __Complicated { */
/*   long foo; */
/*   string bar; */
/*   struct __Complicated *baz; */

/*   typedef ::boost::tuple<> primitives; */

/*   typedef ::boost::tuple<long, string, avro_record> type; */
/* }; */

#include "seq_enum_stringize.hpp"


#define AVRO_STRING "\"string\""
#define AVRO_BYTES "\"bytes\""
#define AVRO_INT "\"int\""
#define AVRO_LONG "\"long\""
#define AVRO_FLOAT "\"float\""
#define AVRO_DOUBLE "\"double\""
#define AVRO_BOOLEAN "\"boolean\""
#define AVRO_NULL "\"null\""

/* AVRO_RECORD(LongList, */
/* 	    (value, AVRO_LONG) */
/* 	    (next, AVRO_UNION(LongList, AVRO_NULL))); */

// 	      {"name": "value", "type": "long"},             // each element has a long
// 	      {"name": "next", "type": ["LongList", "null"]} // optional next element

#define __AVRO_RECORD_TRANSFORM(s, data, elem) \
  "{\"name\": \"" BOOST_PP_STRINGIZE(BOOST_PP_TUPLE_ELEM(2, 0, elem)) "\", \"type\": " BOOST_PP_TUPLE_ELEM(2, 1, elem) "}"

#define AVRO_RECORD(name, seq) \
  "{ \"type\": \"record\", \n" \
  "  \"name\": \"" BOOST_PP_STRINGIZE(name) "\",\n" \
  "  \"fields\" : [\n" \
  "    " SEQ_ENUM_STRINGIZE(BOOST_PP_SEQ_TRANSFORM(__AVRO_RECORD_TRANSFORM, _, seq)) "\n"	\
  "  ]\n" \
  "}"

/* AVRO_ENUM(Suit, (SPADES)(HEARTS)(DIAMONDS)(CLUBS)); */


#define __AVRO_ENUM_TRANSFORM(s, data, elem) "\"" BOOST_PP_STRINGIZE(elem) "\""

#define AVRO_ENUM(name, seq)                                            \
  "{ \"type\": \"enum\",\n"                                             \
  "  \"name\": \"" BOOST_PP_STRINGIZE(name) "\",\n"         	        \
  "  \"symbols\": [" SEQ_ENUM_STRINGIZE(BOOST_PP_SEQ_TRANSFORM(__AVRO_ENUM_TRANSFORM, _, seq)) "]\n" \
  "}"

/* AVRO_ARRAY(AVRO_STRING); */

#define AVRO_ARRAY(type)                                                \
  "{ \"type\": \"array\", \"items\": " type "}"

/* AVRO_MAP(AVRO_LONG); */

#define AVRO_MAP(type)                                                  \
  "{ \"type\": \"map\", \"values\": " type "}"

/* AVRO_UNION((AVRO_STRING)(AVRO_NULL)); */

#define AVRO_UNION(seq)                                                 \
  "[" SEQ_ENUM_STRINGIZE(seq) "]"

/* AVRO_FIXED(md5, 16); */

#define AVRO_FIXED(name, size)						\
  "{ \"type\": \"fixed\", \"size\": " BOOST_PP_STRINGIZE(size) ", \"name\": \"" BOOST_PP_STRINGIZE(name) "\"}"

#define AVRO_TYPE(name) "\"" BOOST_PP_STRINGIZE(name) "\""


const char *schema = 
  AVRO_RECORD(LongList,
	      ((value, AVRO_LONG))
	      ((next, AVRO_UNION((AVRO_TYPE(LongList))
				 (AVRO_NULL)))));


int main(int argc, char **argv)
{
//   std::cout << BOOST_PP_SEQ_TRANSFORM(OP, _, fields) << std::endl;
//   std::cout << AVRO_RECORD(LongList, ((value, AVRO_LONG))((next, AVRO_UNION((AVRO_TYPE(LongList))(AVRO_NULL))))) << std::endl;
//   std::cout << AVRO_ENUM(Suit, (SPADES)(HEARTS)(DIAMONDS)(CLUBS)) << std::endl;
//   std::cout << AVRO_ARRAY(AVRO_STRING) << std::endl;
//   std::cout << AVRO_MAP(AVRO_LONG) << std::endl;
//   std::cout << AVRO_UNION((AVRO_STRING)(AVRO_NULL)) << std::endl;
//   std::cout << AVRO_FIXED(md5, 16) << std::endl;
  std::cout << schema << std::endl;
  return 0;
}
