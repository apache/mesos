#ifndef __STOUT_FATAL_HPP__
#define __STOUT_FATAL_HPP__

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>


/*
 * Like the non-debug version except includes the file name and line
 * number in the output.
 */
#define fatal(fmt...) __fatal(__FILE__, __LINE__, fmt)
inline void __fatal(const char *file, int line, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, " (%s:%u)\n", file, line);
  fflush(stderr);
  va_end(args);
  exit(1);
}


/*
 * Like the non-debug version except includes the file name and line
 * number in the output.
 */
#define fatalerror(fmt...) __fatalerror(__FILE__, __LINE__, fmt)
inline void __fatalerror(const char *file, int line, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, " (%s:%u): ", file, line);
  perror(NULL);
  fflush(stderr);
  va_end(args);
  exit(1);
}

#endif // __STOUT_FATAL_HPP__
