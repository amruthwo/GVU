#ifdef GVU_A30
/*
 * glibc_compat.c — compatibility shim for glibc 2.23 (Miyoo A30 / SpruceOS).
 *
 * The cross-compiler on Debian Bullseye (glibc 2.31) emits calls to
 * fcntl64@GLIBC_2.28 which does not exist in glibc 2.23.
 * Providing a local definition here resolves the symbol statically so
 * the dynamic linker never needs to find it in libc.so.6.
 */

#include <stdarg.h>

/* Bind to the old fcntl@GLIBC_2.4 which exists in glibc 2.23. */
extern int __fcntl_v4(int fd, int cmd, ...)
    __asm__("fcntl@GLIBC_2.4");

/* Provide fcntl64 locally — forward to fcntl@GLIBC_2.4. */
int fcntl64(int fd, int cmd, ...)
{
    va_list args;
    va_start(args, cmd);
    long arg = va_arg(args, long);
    va_end(args);
    return __fcntl_v4(fd, cmd, arg);
}

#endif /* GVU_A30 */
