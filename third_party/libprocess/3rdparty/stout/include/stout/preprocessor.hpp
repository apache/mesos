#ifndef __PROCESS_PREPROCESSOR_HPP__
#define __PROCESS_PREPROCESSOR_HPP__

#include <boost/preprocessor/cat.hpp>

#include <boost/preprocessor/arithmetic/inc.hpp>

#include <boost/preprocessor/facilities/intercept.hpp>

#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_binary_params.hpp>
#include <boost/preprocessor/repetition/enum_trailing_params.hpp>
#include <boost/preprocessor/repetition/repeat.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>

// Provides aliases to a bunch of preprocessor macros useful for
// creating template definitions that have varying number of
// parameters (should be removable with C++-11 variadic templates).

#define CAT BOOST_PP_CAT
#define INC BOOST_PP_INC
#define INTERCEPT BOOST_PP_INTERCEPT
#define ENUM_PARAMS BOOST_PP_ENUM_PARAMS
#define ENUM_BINARY_PARAMS BOOST_PP_ENUM_BINARY_PARAMS
#define ENUM_TRAILING_PARAMS BOOST_PP_ENUM_TRAILING_PARAMS
#define REPEAT BOOST_PP_REPEAT
#define REPEAT_FROM_TO BOOST_PP_REPEAT_FROM_TO

#endif // __PROCESS_PREPROCESSOR_HPP__
