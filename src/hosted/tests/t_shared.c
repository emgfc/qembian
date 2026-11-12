// t_shared: one Linux process per Symbian process, the kernel's data shared between them (a memfd
// mapped at the same fixed address, KKernelDataBase 0xC8000000, in every process) and a futex in it
// that works across processes; plus process_vm_readv for IPC descriptor copies (architecture.md).
#include "hrt.h"

#define KDATA 0xC8000000u
#define KSIZE 0x00100000u
typedef struct { volatile int word; volatile uint32_t magic; volatile int child_saw; char msg[64]; } kdata;

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	int fd = hmemfd("ekern-data");
	HCHECK(fd >= 0, "memfd_create %d", fd);
	HCHECK(hftruncate(fd, KSIZE) == 0, "ftruncate 1 MB");
	kdata *k = hmmap((void *)KDATA, KSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
	HCHECK((uint32_t)k == KDATA, "kernel data at %08x in the parent: %p", KDATA, k);
	if ((uint32_t)k != KDATA) return htest_done();
	k->magic = 0x45504f43;
	static char local[32] = "descriptor in the parent";

	int pid = hfork();
	if (pid == 0) {
		// the child: a fresh mapping at the same address (as a new process would map it)
		hmunmap((void *)KDATA, KSIZE);
		kdata *c = hmmap((void *)KDATA, KSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
		if ((uint32_t)c != KDATA) hexit(2);
		c->child_saw = c->magic == 0x45504f43;
		memcpy((char *)c->msg, "hello from the child", 21);
		c->word = 1;
		hfutex_wake(&c->word, 1);
		while (c->word != 2) hfutex_wait(&c->word, 1);
		hexit(0);
	}
	HCHECK(pid > 0, "fork %d", pid);
	while (k->word == 0) hfutex_wait(&k->word, 0);
	HCHECK(k->child_saw == 1, "child sees the parent's kernel data");
	HCHECK(!strcmp(k->msg, "hello from the child"), "parent sees the child's write: '%s'", k->msg);
	// read the child's copy of `local` (same address, its own copy-on-write page)
	char got[32] = { 0 };
	struct { void *b; uint32_t n; } liov = { got, sizeof got }, riov = { local, sizeof local };
	long n = hsys(SYS_process_vm_readv, pid, &liov, 1, &riov, 1, 0);
	HCHECK(n == sizeof local && !strcmp(got, local), "process_vm_readv from the child: %ld '%s'", n, got);
	k->word = 2;
	hfutex_wake(&k->word, 1);
	HCHECK(hwait(pid) == 0, "child exit 0");
	return htest_done();
}
