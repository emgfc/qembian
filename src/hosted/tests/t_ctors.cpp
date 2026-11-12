// t_ctors: regression for the bug of 2026-11-12: hrt's _start did not run static constructors, so
// C++ globals with constructors (the nanokernel's TheScheduler, TheTimerQ) stayed zero and the first
// timer tick crashed in SDblQue::Add. Checks: a global constructor ran before main, in order, once;
// a constructor that depends on another translation unit-local global sees it initialised.
#include "hrt.h"

static int order[4];
static int norder;

struct Rec {
	int v;
	explicit Rec(int a) : v(a) { order[norder++] = a; }
};
static Rec first(1);
static Rec second(2);
static struct Dep { int seen; Dep() : seen(second.v) {} } dep;

int main(int, char **) {
	HCHECK(norder == 2, "two constructors ran before main (%d)", norder);
	HCHECK(order[0] == 1 && order[1] == 2, "in definition order (%d, %d)", order[0], order[1]);
	HCHECK(dep.seen == 2, "a later constructor sees an earlier global initialised (%d)", dep.seen);
	HCHECK(first.v == 1 && second.v == 2, "values kept");
	return htest_done();
}
