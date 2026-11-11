/* host build of the RI (qembian, egl-vg-host-v2.md): the Khronos scalar types without Symbian's e32def.h */
#ifndef KHRONOS_TYPES_H
#define KHRONOS_TYPES_H
#include <stdint.h>
typedef float khronos_float_t;
typedef int8_t khronos_int8_t;
typedef uint8_t khronos_uint8_t;
typedef int16_t khronos_int16_t;
typedef uint16_t khronos_uint16_t;
typedef int32_t khronos_int32_t;
typedef uint32_t khronos_uint32_t;
typedef int64_t khronos_int64_t;
typedef uint64_t khronos_uint64_t;
#ifndef IMPORT_C
#define IMPORT_C
#endif
#ifndef EXPORT_C
#define EXPORT_C
#endif
#ifndef __SOFTFP
#define __SOFTFP
#endif
#endif
