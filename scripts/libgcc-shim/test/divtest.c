/* Host-side check of the libgcc2.c shim config: compare shim-built __divdi3/__udivdi3/
   __udivmoddi4 (renamed t_*) against the host compiler's 64-bit division. */
#include <stdio.h>
#include <stdlib.h>
long long t_divdi3(long long, long long);
unsigned long long t_udivdi3(unsigned long long, unsigned long long);
unsigned long long t_udivmoddi4(unsigned long long, unsigned long long, unsigned long long *);
static unsigned long long r64(void) {
  unsigned long long v = 0; int i;
  for (i = 0; i < 4; i++) v = (v << 16) ^ (unsigned)(rand() & 0xffff);
  return v >> (rand() % 64);   /* vary magnitude */
}
int main(void) {
  long n, bad = 0; srand(12345);
  for (n = 0; n < 5000000; n++) {
    unsigned long long a = r64(), b = r64(), r; if (!b) continue;
    long long sa = (long long)a, sb = (long long)b;
    if (t_udivdi3(a, b) != a / b) bad++;
    if (t_udivmoddi4(a, b, &r) != a / b || r != a % b) bad++;
    if (!(sb == -1 && sa == (long long)0x8000000000000000ULL) && t_divdi3(sa, sb) != sa / sb) bad++;
  }
  printf("%ld iterations, %ld mismatches\n", n, bad);
  return bad != 0;
}
