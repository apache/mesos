#ifndef CONFIG_HPP
#define CONFIG_HPP

#ifdef __sun__
#define gethostbyname2(name, _) gethostbyname(name)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#endif /* __sun__ */

#ifdef __APPLE__
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#define sendfile(s, fd, offset, size) \
  ({ off_t length = size; \
    sendfile(fd, s, offset, &length, NULL, 0) == 0 \
      ? length \
      : (errno == EAGAIN ? (length > 0 ? length : -1) : -1); })
#endif /* __APPLE__ */

#ifdef __linux__
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#include <sys/sendfile.h>
#define sendfile(s, fd, offset, size) \
  ({ off_t _offset = offset; \
    sendfile(s, fd, &_offset, size) == -1 ? -1 : _offset - offset; })
#endif /* __linux__ */

#endif /* CONFIG_HPP */
