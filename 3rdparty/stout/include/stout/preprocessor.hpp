// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_PREPROCESSOR_HPP__
#define __STOUT_PREPROCESSOR_HPP__

#include <boost/preprocessor/cat.hpp>

#include <boost/preprocessor/arithmetic/inc.hpp>

#include <boost/preprocessor/facilities/intercept.hpp>

#include <boost/preprocessor/repetition/enum_binary_params.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
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
#define ENUM BOOST_PP_ENUM
#define ENUM_TRAILING_PARAMS BOOST_PP_ENUM_TRAILING_PARAMS
#define REPEAT BOOST_PP_REPEAT
#define REPEAT_FROM_TO BOOST_PP_REPEAT_FROM_TO

#endif // __STOUT_PREPROCESSOR_HPP__
