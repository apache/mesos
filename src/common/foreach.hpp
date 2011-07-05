/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FOREACH_HPP
#define FOREACH_HPP

#include <boost/foreach.hpp>

#include <boost/tuple/tuple.hpp>


#define BOOST_FOREACH_PAIR(VARFIRST, VARSECOND, COL)                                            \
    BOOST_FOREACH_PREAMBLE()                                                                    \
    if (boost::foreach_detail_::auto_any_t _foreach_col = BOOST_FOREACH_CONTAIN(COL)) {} else   \
    if (boost::foreach_detail_::auto_any_t _foreach_cur = BOOST_FOREACH_BEGIN(COL)) {} else     \
    if (boost::foreach_detail_::auto_any_t _foreach_end = BOOST_FOREACH_END(COL)) {} else       \
    for (bool _foreach_continue = true, _foreach_onetime = true;                                \
              _foreach_continue && !BOOST_FOREACH_DONE(COL);                                    \
              _foreach_continue ? BOOST_FOREACH_NEXT(COL) : (void)0)                            \
        if  (boost::foreach_detail_::set_false(_foreach_onetime)) {} else                       \
        for (VARFIRST = BOOST_FOREACH_DEREF(COL).first;                                         \
	     !_foreach_onetime;                                                                 \
	     _foreach_onetime = true)                                                           \
            if  (boost::foreach_detail_::set_false(_foreach_continue)) {} else                  \
            for (VARSECOND = BOOST_FOREACH_DEREF(COL).second;                                   \
		 !_foreach_continue;                                                            \
		 _foreach_continue = true)

#define foreach BOOST_FOREACH
#define foreachpair BOOST_FOREACH_PAIR

#define foreachkey(VAR, COL)                    \
  foreachpair (VAR, boost::tuples::ignore, COL)

#define foreachvalue(VAR, COL)                  \
  foreachpair (boost::tuples::ignore, VAR, COL)

#endif // __FOREACH_HPP__
