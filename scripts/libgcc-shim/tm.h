/* Minimal target description for compiling selected GCC 4.4.1 libgcc2.c routines
   (L_udivmoddi4, L_divdi3, L_udivdi3) for arm-none-symbianelf: ILP32, little-endian,
   64-bit long long, soft-float doubles. Values match gcc/config/arm/arm.h + defaults.h. */
#define BITS_PER_UNIT 8
#define UNITS_PER_WORD 4
#define MIN_UNITS_PER_WORD 4
#define SHORT_TYPE_SIZE 16
#define INT_TYPE_SIZE 32
#define LONG_TYPE_SIZE 32
#define LONG_LONG_TYPE_SIZE 64
#define FLOAT_TYPE_SIZE 32
#define DOUBLE_TYPE_SIZE 64
#define LONG_DOUBLE_TYPE_SIZE 64
#define WORDS_BIG_ENDIAN 0
#define BYTES_BIG_ENDIAN 0

/* From gcc/config/arm/bpabi.h: give __muldi3 its EABI name __aeabi_lmul. */
#ifdef L_muldi3
#define RENAME_LIBRARY_SET ".set"
#define RENAME_LIBRARY(GCC_NAME, AEABI_NAME)		\
  __asm__ (".globl\t__aeabi_" #AEABI_NAME "\n"		\
	   RENAME_LIBRARY_SET "\t__aeabi_" #AEABI_NAME 	\
	     ", __" #GCC_NAME "\n");
#define DECLARE_LIBRARY_RENAMES RENAME_LIBRARY (muldi3, lmul)
#endif
