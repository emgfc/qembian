// Hosted EKA2: the e32atomics.h API (EPL header e32atomics.h, Nokia) on compiler atomics. The ARM build
// uses common/arm/atomics.cia, the Win32 build x86 lock-prefixed code; on a hosted ARMv7+/v8 process
// the compiler's LDREX/STREX(D) sequences are the natural implementation. Every function, all widths.
#include <e32def.h>
#include <e32atomics.h>

#define ORD_rlx __ATOMIC_RELAXED
#define ORD_acq __ATOMIC_ACQUIRE
#define ORD_rel __ATOMIC_RELEASE
#define ORD_ord __ATOMIC_SEQ_CST
#define CAS_FAIL_rlx __ATOMIC_RELAXED
#define CAS_FAIL_acq __ATOMIC_ACQUIRE
#define CAS_FAIL_rel __ATOMIC_RELAXED
#define CAS_FAIL_ord __ATOMIC_SEQ_CST

#define RMW(N, T, S, op, bop) \
	EXPORT_C T __e32_atomic_##op##_##S##N(volatile TAny* a, T v) { return __atomic_fetch_##bop((volatile T*)a, v, ORD_##S); }

#define TAS(N, ST, S) \
	EXPORT_C ST __e32_atomic_tas_##S##N(volatile TAny* a, ST t, ST u, ST v) \
		{ ST o = *(volatile ST*)a; while (!__atomic_compare_exchange_n((volatile ST*)a, &o, (ST)(o >= t ? o + u : o + v), true, ORD_##S, CAS_FAIL_##S)) {} return o; }

#define WIDTH(N, T, ST) \
	EXPORT_C T __e32_atomic_load_acq##N(const volatile TAny* a) { return __atomic_load_n((const volatile T*)a, __ATOMIC_ACQUIRE); } \
	EXPORT_C T __e32_atomic_store_rel##N(volatile TAny* a, T v) { __atomic_store_n((volatile T*)a, v, __ATOMIC_RELEASE); return v; } \
	EXPORT_C T __e32_atomic_store_ord##N(volatile TAny* a, T v) { __atomic_store_n((volatile T*)a, v, __ATOMIC_SEQ_CST); return v; }

extern "C" {
#undef EXPORT_C
#define EXPORT_C

#define ORDS(N, T, ST) ALL_ORD2(N, T, rlx) ALL_ORD2(N, T, acq) ALL_ORD2(N, T, rel) ALL_ORD2(N, T, ord) \
	TAS(N, ST, rlx) TAS(N, ST, acq) TAS(N, ST, rel) TAS(N, ST, ord)
#define ALL_ORD2(N, T, S) \
	T __e32_atomic_swp_##S##N(volatile TAny* a, T v) { return __atomic_exchange_n((volatile T*)a, v, ORD_##S); } \
	TBool __e32_atomic_cas_##S##N(volatile TAny* a, T* q, T v) \
		{ return __atomic_compare_exchange_n((volatile T*)a, q, v, false, ORD_##S, CAS_FAIL_##S); } \
	RMW(N, T, S, add, add) RMW(N, T, S, and, and) RMW(N, T, S, ior, or) RMW(N, T, S, xor, xor) \
	T __e32_atomic_axo_##S##N(volatile TAny* a, T u, T v) \
		{ T o = *(volatile T*)a; while (!__atomic_compare_exchange_n((volatile T*)a, &o, (T)((o & u) ^ v), true, ORD_##S, CAS_FAIL_##S)) {} return o; } \
	T __e32_atomic_tau_##S##N(volatile TAny* a, T t, T u, T v) \
		{ T o = *(volatile T*)a; while (!__atomic_compare_exchange_n((volatile T*)a, &o, (T)(o >= t ? o + u : o + v), true, ORD_##S, CAS_FAIL_##S)) {} return o; }

WIDTH(8, TUint8, TInt8)    ORDS(8, TUint8, TInt8)
WIDTH(16, TUint16, TInt16) ORDS(16, TUint16, TInt16)
WIDTH(32, TUint32, TInt32) ORDS(32, TUint32, TInt32)
WIDTH(64, TUint64, TInt64) ORDS(64, TUint64, TInt64)

void __e32_memory_barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
void __e32_io_completion_barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
TInt __e32_find_ms1_32(TUint32 v) { return v ? 31 - __builtin_clz(v) : -1; }
TInt __e32_find_ls1_32(TUint32 v) { return v ? __builtin_ctz(v) : -1; }
TInt __e32_bit_count_32(TUint32 v) { return __builtin_popcount(v); }
TInt __e32_find_ms1_64(TUint64 v) { return v ? 63 - __builtin_clzll(v) : -1; }
TInt __e32_find_ls1_64(TUint64 v) { return v ? __builtin_ctzll(v) : -1; }
TInt __e32_bit_count_64(TUint64 v) { return __builtin_popcountll(v); }
}
