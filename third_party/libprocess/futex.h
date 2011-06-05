#ifndef FUTEX_H
#define FUTEX_H

#include <errno.h>
#include <limits.h>
#include <unistd.h>

#include <linux/futex.h>
#include <sys/syscall.h>


inline void futex_wait(void *futex, int comparand)
{
  int r = syscall(SYS_futex, futex, FUTEX_WAIT, comparand, NULL, NULL, 0);
  if (!(r == 0 || r == EWOULDBLOCK ||
      (r == -1 && (errno == EAGAIN || errno == EINTR)))) {
    fprintf(stderr, "futex_wait failed");
    exit(1);
  }
}


inline void futex_wakeup_one(void *futex)
{
  int r = syscall(SYS_futex, futex, FUTEX_WAKE, 1, NULL, NULL, 0);
  if (!(r == 0 || r == 1)) {
    fprintf(stderr, "futex_wakeup_one failed");
    exit(1);
  }
}


inline void futex_wakeup_all(void *futex)
{
  int r = syscall(SYS_futex, futex, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
  if (!(r >= 0)) {
    fprintf(stderr, "futex_wakeup_all failed");
    exit(1);
  }
}


#endif /* FUTEX_H */
