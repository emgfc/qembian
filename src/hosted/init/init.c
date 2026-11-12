// htest init: PID 1 of the hosted-EKA2 test guest (aarch64, static). Mounts the pseudo file systems,
// sets the 32-bit compat knobs the hosted kernel needs (abi.swp, abi.cp15_barrier), runs the test
// programs in /t (or those named by `htest=a,b` on the kernel command line) each with a time limit,
// prints machine-readable result lines on the console and powers the machine off.
//   HTEST-BEGIN <name>
//   ... the test's own output ("  ok ...", "  FAIL ...", "  note ...")
//   HTEST-RESULT <name> PASS|FAIL|SKIP|TIMEOUT|SIGNAL <code> <ms>   (exit 77 = SKIP)
//   HTEST-SUMMARY pass=N fail=M
// `htest.shell=1` is not offered: there is no shell in the image (no busybox).
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void put(const char *path, const char *v) {
	int fd = open(path, O_WRONLY);
	if (fd < 0) { printf("htest-init: cannot open %s\n", path); return; }
	if (write(fd, v, strlen(v)) < 0) printf("htest-init: cannot write %s = %s\n", path, v);
	close(fd);
}

static long ms_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char cmdline[4096];
static const char *arg(const char *key) {
	size_t k = strlen(key);
	for (char *p = cmdline; (p = strstr(p, key)); p += k)
		if ((p == cmdline || p[-1] == ' ') && p[k] == '=') return p + k + 1;
	return NULL;
}
static int listed(const char *list, const char *name) {
	if (!list) return 1;
	size_t n = strlen(name);
	for (const char *p = list; *p && *p != ' ';) {
		const char *e = p;
		while (*e && *e != ',' && *e != ' ') e++;
		if ((size_t)(e - p) == n && !strncmp(p, name, n)) return 1;
		p = *e == ',' ? e + 1 : e;
	}
	return 0;
}


// ---- phone mode (htest --phone): a tiny `top` on the console, every 2 s, each line prefixed "LXTOP " and
// the block closed by "LXTOP-END" (tools/hosted/phone.py shows it on F12). It shows what the phone really is:
// a Linux machine whose one process "symbian" carries every Symbian thread as a Linux thread (the hosted
// kernel names them after the Symbian threads).
static int readfile(const char *path, char *buf, int max) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	int n = read(fd, buf, max - 1);
	close(fd);
	if (n < 0) n = 0;
	buf[n] = 0;
	return n;
}

// /proc/.../stat: comm (in parentheses, may contain anything), state, utime + stime in ticks
static int statline(const char *path, char *comm, char *state, unsigned long *ticks) {
	char b[1024];
	if (readfile(path, b, sizeof b) <= 0) return -1;
	char *l = strchr(b, '('), *r = strrchr(b, ')');
	if (!l || !r) return -1;
	int n = r - l - 1; if (n > 15) n = 15;
	memcpy(comm, l + 1, n); comm[n] = 0;
	*state = r[2];
	char *p = r + 2; unsigned long ut = 0, st = 0;
	for (int f = 3; f <= 15 && p; f++) {			// field 3 = state ... 14 utime, 15 stime
		if (f == 14) ut = strtoul(p, NULL, 10);
		if (f == 15) st = strtoul(p, NULL, 10);
		p = strchr(p, ' '); if (p) p++;
	}
	*ticks = ut + st;
	return 0;
}

struct ttask { int id; unsigned long ticks; char comm[16]; char state; long d; };
static struct ttask prev[512], cur[512];
static int nprev;

static long delta(int id, unsigned long ticks) {
	for (int i = 0; i < nprev; i++) if (prev[i].id == id) return (long)(ticks - prev[i].ticks);
	return 0;
}
static int bycpu(const void *a, const void *b) {
	const struct ttask *x = a, *y = b;
	return y->d != x->d ? (y->d > x->d ? 1 : -1) : x->id - y->id;
}

static void lxtop(void) {
	const int period = 2;
	long hz = sysconf(_SC_CLK_TCK);
	struct utsname u; uname(&u);
	static char out[16384];
	for (;;) {
		sleep(period);
		int o = 0, ncur = 0, kthreads = 0, symbian = 0;
		char b[512];
		readfile("/proc/uptime", b, sizeof b);
		long up = atol(b);
		char load[64] = ""; readfile("/proc/loadavg", load, sizeof load);
		char *sp = load; for (int k = 0; k < 3 && sp; k++) sp = strchr(sp + 1, ' '); if (sp) *sp = 0;
		char mem[4096]; readfile("/proc/meminfo", mem, sizeof mem);
		char *mt = strstr(mem, "MemTotal:"), *ma = strstr(mem, "MemAvailable:");
		long tot = mt ? atol(mt + 9) : 0, av = ma ? atol(ma + 13) : 0;
		o += snprintf(out + o, sizeof out - o, "LXTOP %s %s %s  up %ldm%02lds  load %s\n", u.sysname, u.release,
		              u.machine, up / 60, up % 60, load);
		o += snprintf(out + o, sizeof out - o, "LXTOP Mem: %ld MB used of %ld MB\n", (tot - av) / 1024, tot / 1024);
		o += snprintf(out + o, sizeof out - o, "LXTOP\nLXTOP   PID S  CPU%%  PROCESS\n");
		DIR *d = opendir("/proc");
		struct dirent *e;
		while (d && (e = readdir(d))) {
			int pid = atoi(e->d_name);
			if (pid <= 0) continue;
			char path[64], comm[16], st; unsigned long t;
			snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
			if (readfile(path, b, sizeof b) <= 0) { kthreads++; continue; }	// kernel threads have no cmdline
			snprintf(path, sizeof path, "/proc/%d/stat", pid);
			if (statline(path, comm, &st, &t)) continue;
			if (ncur < 512) { cur[ncur] = (struct ttask){ -pid, t, "", st, 0 }; ncur++; }
			o += snprintf(out + o, sizeof out - o, "LXTOP %5d %c %5.1f  %s%s\n", pid, st,
			              100.0 * delta(-pid, t) / (hz * period), comm,
			              pid == 1 ? "  (init)" : !strcmp(comm, "symbian") ? "  <- the whole phone" : "");
			if (!strcmp(comm, "symbian")) symbian = pid;
		}
		if (d) closedir(d);
		o += snprintf(out + o, sizeof out - o, "LXTOP   + %d Linux kernel threads\n", kthreads);
		if (symbian) {
			char path[64];
			snprintf(path, sizeof path, "/proc/%d/task", symbian);
			struct ttask th[400]; int nt = 0, running = 0;
			d = opendir(path);
			while (d && (e = readdir(d)) && nt < 400) {
				int tid = atoi(e->d_name);
				if (tid <= 0) continue;
				char sp2[80];
				snprintf(sp2, sizeof sp2, "/proc/%d/task/%d/stat", symbian, tid);
				if (statline(sp2, th[nt].comm, &th[nt].state, &th[nt].ticks)) continue;
				th[nt].id = tid;
				th[nt].d = delta(tid, th[nt].ticks);
				if (th[nt].state == 'R') running++;
				if (ncur < 512) cur[ncur++] = th[nt];
				nt++;
			}
			if (d) closedir(d);
			qsort(th, nt, sizeof th[0], bycpu);
			o += snprintf(out + o, sizeof out - o, "LXTOP\nLXTOP Symbian threads = Linux threads of pid %d: %d (%d running)\n",
			              symbian, nt, running);
			o += snprintf(out + o, sizeof out - o, "LXTOP   TID S  CPU%%  THREAD\n");
			for (int i = 0; i < nt && i < 24; i++)
				o += snprintf(out + o, sizeof out - o, "LXTOP %5d %c %5.1f  %s\n", th[i].id, th[i].state,
				              100.0 * th[i].d / (hz * period), th[i].comm);
		}
		o += snprintf(out + o, sizeof out - o, "LXTOP-END\n");
		memcpy(prev, cur, ncur * sizeof cur[0]);
		nprev = ncur;
		if (write(1, out, o) < 0) {}
	}
}

static int cmp(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

int main(void) {
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	// the phone ROM share (htest --rom): read-only 9p, absent when no ROM was given
	mkdir("/rom", 0755);
	if (mount("rom", "/rom", "9p", MS_RDONLY, "trans=virtio,version=9p2000.L,cache=loose") == 0)
		printf("htest-init: ROM share mounted at /rom\n");
	mkdir("/tmp", 0777);
	mkdir("/seed", 0755);
	if (mount("seed", "/seed", "9p", MS_RDONLY, "trans=virtio,version=9p2000.L,cache=loose") == 0)
		printf("htest-init: drive seeds mounted at /seed\n");
	mkdir("/out", 0755);
	if (mount("out", "/out", "9p", 0, "trans=virtio,version=9p2000.L") == 0)
		printf("htest-init: host output share at /out\n");
	mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
	setvbuf(stdout, NULL, _IONBF, 0);

	int fd = open("/proc/cmdline", O_RDONLY);
	if (fd >= 0) { ssize_t n = read(fd, cmdline, sizeof cmdline - 1); if (n > 0) cmdline[n] = 0; close(fd); }
	char *nl = strchr(cmdline, '\n'); if (nl) *nl = 0;

	put("/proc/sys/abi/swp", "1");                  // emulate SWP/SWPB (ARMv5 EUSER)
	const char *cp15 = arg("htest.cp15");
	put("/proc/sys/abi/cp15_barrier", cp15 ? (char[]){ cp15[0], 0 } : "1");
	put("/proc/sys/kernel/yama/ptrace_scope", "0");

	printf("HTEST-START kernel=%s\n", cmdline);

	if (strstr(cmdline, "htest.env.PHONE=") && fork() == 0) {
		prctl(PR_SET_NAME, "lxtop");
		lxtop();
	}

	const char *only = arg("htest");
	long timeout_ms = arg("htest.timeout") ? atol(arg("htest.timeout")) * 1000 : 60000;
	DIR *d = opendir("/t");
	char *names[256]; int n = 0;
	struct dirent *e;
	while (d && (e = readdir(d)) && n < 256)
		if (!strchr(e->d_name, '.') && listed(only, e->d_name)) names[n++] = strdup(e->d_name);   // no data files
	if (d) closedir(d);
	qsort(names, n, sizeof names[0], cmp);

	int pass = 0, fail = 0;
	for (int i = 0; i < n; i++) {
		char path[300];
		snprintf(path, sizeof path, "/t/%s", names[i]);
		printf("HTEST-BEGIN %s\n", names[i]);
		// per-test time limit: htest.t.<name>=SECONDS on the command line overrides htest.timeout
		char key[300];
		snprintf(key, sizeof key, "htest.t.%s", names[i]);
		long limit_ms = arg(key) ? atol(arg(key)) * 1000 : timeout_ms;
		long t0 = ms_now();
		pid_t pid = fork();
		if (pid == 0) {
			chdir("/tmp");
			char *argv[] = { path, NULL };
			// htest.env.NAME=VALUE on the kernel command line -> NAME=VALUE in the test's environment
			char *envp[32]; int ne = 0;
			envp[ne++] = "HTEST=1";
			for (char *q = cmdline; (q = strstr(q, "htest.env.")) && ne < 31; ) {
				q += 10;
				char *e = q; while (*e && *e != ' ') e++;
				envp[ne++] = strndup(q, e - q);
				q = e;
			}
			envp[ne] = NULL;
			execve(path, argv, envp);
			printf("  FAIL execve %s: %m\n", path);
			_exit(127);
		}
		int st = 0, done = 0;
		const char *res = "TIMEOUT"; int code = -1;
		while (ms_now() - t0 < limit_ms) {
			pid_t r = waitpid(pid, &st, WNOHANG);
			if (r == pid) { done = 1; break; }
			usleep(2000);
		}
		if (!done) {
			// time out: ask for a state dump first (hosted kernel tests print every thread on SIGQUIT)
			kill(pid, SIGQUIT);
			for (int k = 0; k < 300 && waitpid(pid, &st, WNOHANG) != pid; k++) usleep(10000);
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
		}
		else if (WIFEXITED(st)) { code = WEXITSTATUS(st); res = code == 77 ? "SKIP" : code ? "FAIL" : "PASS"; }
		else { code = WTERMSIG(st); res = "SIGNAL"; }
		if (!strcmp(res, "PASS") || !strcmp(res, "SKIP")) pass++; else fail++;
		printf("HTEST-RESULT %s %s %d %ld\n", names[i], res, code, ms_now() - t0);
	}
	printf("HTEST-SUMMARY pass=%d fail=%d\n", pass, fail);
	sync();
	reboot(RB_POWER_OFF);
	return 0;
}
