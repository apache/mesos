/*
 * Below are some esoteric macros used to "count" the number of
 * arguments passed to a variadic macro. See comments below.
 *
 * It is rather frustrating that something as easy as determining the
 * number of arguments requires such hackery.
 *
 * TODO: A known bug is that if the first argument is the empty string
 * (i.e. it is not included as in (, x) ) then the macro will return a
 * count of 0.
 *
 * Benjamin Hindman (benh@berkeley.edu)
 */

#ifndef VARIADIC_HPP
#define VARIADIC_HPP

#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/control.hpp>

/* Some helpful utility macros not found in boost/preprocessor/*. */
#define HEAD_IMPL(hd, ...) hd
#define HEAD(...) HEAD_IMPL(__VA_ARGS__, )
#define EAT(...)

/*
 * We can fairly easily count the number of arguments provided if
 * there is at least one argument by a nifty shifting technique (see
 * the macro below). Just as many BOOST_PP_* macros have limits, this
 * macro has a 64 argument limit.
 *
 * References:
 *
 * Counting greater than or equal to one argument up to some limit:
 * http://groups.google.com/group/comp.std.c/browse_thread/thread/77ee8c8f92e4a3fb/346fc464319b1ee5
 */

#define VA_ARGS_NTH(                       \
  _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, \
  _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
  _21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
  _31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
  _41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
  _51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
  _61,_62,_63, n,...) n

#define VA_ARGS_SEQ()            \
  63,62,61,60,                   \
  59,58,57,56,55,54,53,52,51,50, \
  49,48,47,46,45,44,43,42,41,40, \
  39,38,37,36,35,34,33,32,31,30, \
  29,28,27,26,25,24,23,22,21,20, \
  19,18,17,16,15,14,13,12,11,10, \
  9,8,7,6,5,4,3,2,1,0

#define VA_ARGS_COUNT_IMPL(...) VA_ARGS_NTH(__VA_ARGS__)

/* Note: Calling VA_ARGS_COUNT without args violates 6.10.3p4 of ISO C99. */
#define VA_ARGS_COUNT(...) VA_ARGS_COUNT_IMPL(__VA_ARGS__, VA_ARGS_SEQ())

/*
 * Probably the trickiest macro here is IS_EMPTY which relies on
 * numerous other macros. Ultimately, we are trying to test for two
 * possibilities, either the empty string or not. But the string
 * itself may be not just a string but a parenthesized string, for
 * example, '(a,b)' ... consider: ((a,b), c). To handle this we use
 * IS_EMPTY_OR_PARENS call to try and check for the empty string or
 * parenthesized string, i.e. we have to allow for the possibility of
 * the argument being a parenthesized string (hence the INTER,
 * POSITION, and INTERITION macros). Since the IS_EMPTY_OR_PARENS
 * call, however, treats the empty string and a parenthesized string
 * equivalently (which they are not since one means no arguments and
 * the other means at least one argument) , we use the
 * IS_EMPTY_NO_PARENS call to check and see if the string is a
 * parenthesized string. We take the complement of this result (in our
 * ultimate BOOST_PP_BITAND) in order to correctly determine if the
 * string is empty or not.
 *
 * Note that this macro is not complete. The macro relies on
 * concatenating strings as a trick to check for the empty string. The
 * macro fails, therefore, if the argument is not concatenable, for
 * example '/', or '^', as concatenating these with other strings is
 * illegal. You can overcome this limitation, if you choose, by
 * wrapping each of your arguments in an extra set of parens.
 *
 * References:
 *
 * Detecting if an argument list is empty:
 * http://www.velocityreviews.com/forums/t444552-behavior-of-variadic-macro.html
 * (Note that my version actually solves the problem listed in this
 * thread relating to possible expansion of original arguments that
 * are themselves function macros!)
 */

#define TST_(...) 1
#define RET_TST_ 0,
#define RET_1 1,

#define TST_RET(a) HEAD(BOOST_PP_CAT(RET_, TST_ a))

#define INTER(...) POS
#define POSITION ()
#define INTERITION ()

#define IS_EMPTY_OR_PARENS(a) TST_RET(BOOST_PP_CAT(INTER a, ITION))

#define IS_EMPTY_NO_PARENS(a) TST_RET(a)

#define IS_EMPTY(a) \
  BOOST_PP_BITAND(IS_EMPTY_OR_PARENS(a), BOOST_PP_COMPL(IS_EMPTY_NO_PARENS(a)))

/*
 * The VA_NUM_ARGS macro NUM_ARGS_IMPL in case __VA_ARGS__ has
 * side-effects ... yup, you read that correctly. Macros might have
 * side-effects so we only want to evaluate them once.
 */
#define VA_ARGS_NUM_IMPL(args) \
  BOOST_PP_IIF(IS_EMPTY(HEAD args), 0 EAT, VA_ARGS_COUNT) args

#define VA_ARGS_NUM(...) VA_ARGS_NUM_IMPL((__VA_ARGS__))

#define BOOST_PP_VA_ARGS_TO_ARRAY_IMPL(n, ...) (n, (__VA_ARGS__))

#define BOOST_PP_VA_ARGS_TO_ARRAY(...) \
  BOOST_PP_VA_ARGS_TO_ARRAY_IMPL(VA_ARGS_NUM(__VA_ARGS__), __VA_ARGS__)


// VA_ARGS_NUM()
// VA_ARGS_NUM(())
// VA_ARGS_NUM(a)
// VA_ARGS_NUM(a,b)
// VA_ARGS_NUM(a,b,c)
// VA_ARGS_NUM((a),b,c)
// VA_ARGS_NUM((a),(b),c)
// VA_ARGS_NUM(((a),(b)),c)


#endif /* VARIADIC_HPP */
