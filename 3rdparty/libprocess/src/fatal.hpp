/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

/*
 * Basic perror + exit routines.
 *
 * Contributed by Benjamin Hindman <benh@berkeley.edu>, 2008.
 */

#ifndef __FATAL_HPP__
#define __FATAL_HPP__

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Like the non-debug version except includes the file name and line
 * number in the output.
 */
#define fatal(fmt...) __fatal(__FILE__, __LINE__, fmt)
void __fatal(const char *file, int line, const char *fmt, ...);

/*
 * Like the non-debug version except includes the file name and line
 * number in the output.
 */
#define fatalerror(fmt...) __fatalerror(__FILE__, __LINE__, fmt)
void __fatalerror(const char *file, int line, const char *fmt, ...);

#endif // __FATAL_HPP__
