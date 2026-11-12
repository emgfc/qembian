// t_hello: the 32-bit freestanding runtime itself: argv, write, getpid, 64-bit division, printf.
#include "hrt.h"

int main(int argc, char **argv) {
	HCHECK(argc >= 1 && argv[0] && argv[0][0], "argv[0] = %s", argc >= 1 ? argv[0] : "?");
	HCHECK(hgetpid() > 0, "getpid %d", hgetpid());
	volatile uint64_t a = 0x123456789abcdefull, b = 1000003;
	HCHECK(a / b == 81985283260ull && a % b == 637115ull, "64-bit udivmod %llu r %llu", (uint64_t)(a / b), (uint64_t)(a % b));
	char buf[64];
	hsnprintf(buf, sizeof buf, "%d %u %x %08x %s %lld", -5, 7u, 0xabcu, 0x12u, "s", -1234567890123ll);
	HCHECK(!strcmp(buf, "-5 7 abc 00000012 s -1234567890123"), "printf: '%s'", buf);
	hsnprintf(buf, sizeof buf, "[%.*s|%.2s|%5s]", 3, "abcdef", "xyz", "ab");
	HCHECK(!strcmp(buf, "[abc|xy|   ab]"), "printf precision: '%s'", buf);
	uint32_t sp; __asm__("mov %0, sp" : "=r"(sp));
	htest_note("stack at %p, code at %p", (void *)sp, (void *)main);
	return htest_done();
}
