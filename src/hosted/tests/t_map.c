// t_map: the EKA2 address-space regions must be mappable at their fixed addresses in a 32-bit
// process (docs/hosted-eka2/architecture.md): multiple/flexible memory model, mmboot.h constants.
#include "hrt.h"

static void show_maps(void) {
	char buf[4096];
	int fd = hsys(SYS_openat, -100, "/proc/self/maps", 0, 0);
	if (fd < 0) return;
	int n = hsys(SYS_read, fd, buf, sizeof buf - 1);
	hsys(SYS_close, fd);
	if (n > 0) { buf[n] = 0; hprintf("--- /proc/self/maps\n%s---\n", buf); }
}

static const struct { const char *name; uint32_t base, size; } regions[] = {
	{ "user local data (KUserLocalDataBase)", 0x00400000u, 0x10000000u },
	{ "shared data 2GB model (KUserSharedDataBase2GB)", 0x40000000u, 0x40000000u },
	{ "ROM (KRomLinearBase)", 0x80000000u, 0x10000000u },
	{ "user global data (KUserGlobalDataBase)", 0x90000000u, 0x10000000u },
	{ "RAM drive (KRamDriveStartAddress)", 0xA0000000u, 0x20000000u },
	{ "kernel data (KKernelDataBase)", 0xC8000000u, 0x01200000u },
	{ "flexible super page (KSuperPageLinAddr)", 0xF0000000u, 0x00100000u },
};

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	for (unsigned i = 0; i < sizeof regions / sizeof regions[0]; i++) {
		uint32_t b = regions[i].base, n = regions[i].size;
		void *p = hmmap((void *)b, n, PROT_READ | PROT_WRITE,
		                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
		int ok = (uint32_t)p == b;
		if (ok) {     // touch first and last page
			volatile uint32_t *f = (uint32_t *)b, *l = (uint32_t *)(b + n - 4);
			*f = 0x5eb0a5e0u; *l = ~0x5eb0a5e0u;
			ok = *f == 0x5eb0a5e0u && *l == ~0x5eb0a5e0u;
		}
		HCHECK(ok, "map %08x..%08x %s -> %p", b, b + n, regions[i].name, p);
	}
	show_maps();
	// the top of the 32-bit space: what is left for Linux (stack, vdso, sigpage, kuser helpers)
	void *top = hmmap((void *)0xFF000000u, 0x00800000u, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	htest_note("0xff000000 (8 MB) mappable: %s", (uint32_t)top == 0xFF000000u ? "yes" : "no");
	return htest_done();
}
