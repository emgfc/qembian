/* Force-included into the Android QEMU build (android/native/build.sh --extra-cflags): POSIX bits bionic
   lacks. The functions are defined in android/native/syborg_android.c and fail cleanly (ENOSYS). */
#ifndef SYBORG_ANDROID_COMPAT_H
#define SYBORG_ANDROID_COMPAT_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
int shm_open(const char *name, int flags, mode_t mode);
int shm_unlink(const char *name);
#ifdef __cplusplus
}
#endif
#endif
