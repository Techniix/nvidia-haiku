/* For checking the test on a non-Haiku host: make ioctl() call
 * __ioctl(fd, op, argument, size), as Haiku's C++ headers do (see
 * headers/posix/unistd.h), so the wrapper for that path is exercised. */
#ifdef __cplusplus
#include <stddef.h>
#include <unistd.h>
#include <sys/ioctl.h>
extern "C" int __ioctl(int fd, unsigned long op, void *argument, size_t size);
static inline int haiku_ioctl(int fd, unsigned long op, void *argument = NULL,
	size_t size = 0)
{
	return __ioctl(fd, op, argument, size);
}
#define ioctl haiku_ioctl
#endif
