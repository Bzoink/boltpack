/*
 * Test-only <sys/xattr.h> for the macOS CLI runtime shim.  The Apple calls are
 * renamed so they cannot clash with the Linux libc functions of the same name;
 * test/macosx_shim/apple_shim.c implements them over Linux user.* attributes.
 */
#ifndef BOLTPACK_SHIM_SYS_XATTR_H
#define BOLTPACK_SHIM_SYS_XATTR_H

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

#define XATTR_NOFOLLOW 1
#define XATTR_RESOURCEFORK_NAME "com.apple.ResourceFork"

ssize_t apple_listxattr(const char *path, char *namebuf, size_t size,
                        int options);
ssize_t apple_getxattr(const char *path, const char *name, void *value,
                       size_t size, unsigned position, int options);
int apple_setxattr(const char *path, const char *name, const void *value,
                   size_t size, unsigned position, int options);
int apple_fsetxattr(int fd, const char *name, const void *value, size_t size,
                    unsigned position, int options);

#define listxattr apple_listxattr
#define getxattr apple_getxattr
#define setxattr apple_setxattr
#define fsetxattr apple_fsetxattr

#endif
