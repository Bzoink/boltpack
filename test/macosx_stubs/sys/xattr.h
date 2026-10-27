#ifndef BOLTPACK_TEST_SYS_XATTR_H
#define BOLTPACK_TEST_SYS_XATTR_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

#define XATTR_NOFOLLOW 1
#define XATTR_RESOURCEFORK_NAME "com.apple.ResourceFork"

ssize_t getxattr(const char *path, const char *name, void *value,
                 size_t size, uint32_t position, int options);
int setxattr(const char *path, const char *name, const void *value,
             size_t size, uint32_t position, int options);
int fsetxattr(int fd, const char *name, const void *value, size_t size,
              uint32_t position, int options);

#endif
