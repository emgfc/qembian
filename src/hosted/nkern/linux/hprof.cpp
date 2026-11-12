// Hosted EKA2: a sampling profiler for speed work (htest --env PROFILE=1). ITIMER_PROF fires every 1 ms of
// process CPU time (user and system); the handler records the interrupted pc. HostedProfileReport prints
// the split by region and the hottest addresses (symbolise with tools/hosted/profsym.py). Original work
// for qembian.
#include "nk_priv.h"

static const TInt KMaxSamples = 1 << 20;
static TUint32* Samples;
static volatile TInt SampleCount;

static void ProfHandler(int, hsiginfo*, hucontext* aUc)
	{
	TInt i = __atomic_fetch_add(&SampleCount, 1, __ATOMIC_RELAXED);
	if (i < KMaxSamples)
		Samples[i] = aUc->mc.pc | ((aUc->mc.cpsr >> 5) & 1);		// bit 0: Thumb
	}

void HostedProfileStart()
	{
	Samples = (TUint32*)hmmap(0, KMaxSamples * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(Samples))
		return;
	hsigaction(27 /* SIGPROF */, ProfHandler, SA_RESTART);
	struct { TInt32 is, iu, vs, vu; } it = { 0, 1000, 0, 1000 };		// interval and value: 1 ms
	hsys(104 /* setitimer */, 2 /* ITIMER_PROF */, &it, 0);
	}

static const char* Region(TUint32 pc)
	{
	if (pc >= 0xd0000000u && pc < 0xd1000000u) return "kernel";
	if (pc >= 0x80000000u && pc < 0x84000000u) return "rom";
	if (pc >= 0x70000000u && pc < 0x78000000u) return "ramcode";
	if (pc >= 0xffff0000u) return "vectors";
	return "other";
	}

void HostedProfileReport()
	{
	TInt n = SampleCount < KMaxSamples ? SampleCount : KMaxSamples;
	if (!n)
		return;
	static const char* const KNames[] = { "kernel", "rom", "ramcode", "vectors", "other" };
	for (TInt r = 0; r < 5; ++r)
		{
		TInt c = 0;
		for (TInt i = 0; i < n; ++i)
			c += Region(Samples[i] & ~1u) == KNames[r];
		hprintf("PROF region %s %d samples (%d%%)\n", KNames[r], c, c * 100 / n);
		}
	// the hottest addresses: sort a copy (in place: the run is over), count runs
	TUint32* s = Samples;
	for (TInt gap = n / 2; gap > 0; gap /= 2)		// shell sort
		for (TInt i = gap; i < n; ++i)
			for (TInt j = i; j >= gap && s[j - gap] > s[j]; j -= gap)
				{
				TUint32 t = s[j]; s[j] = s[j - gap]; s[j - gap] = t;
				}
	// runs of equal pcs as (pc, count) pairs in their own buffer; then the 60 largest
	TUint32* r2 = (TUint32*)hmmap(0, n * 8 + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(r2))
		return;
	TInt runs = 0;
	for (TInt i = 0; i < n; )
		{
		TInt j = i;
		while (j < n && s[j] == s[i]) ++j;
		r2[2 * runs] = s[i];
		r2[2 * runs + 1] = j - i;
		++runs;
		i = j;
		}
	for (TInt k = 0; k < 60; ++k)
		{
		TInt best = -1;
		for (TInt r = 0; r < runs; ++r)
			if (r2[2 * r + 1] && (best < 0 || r2[2 * r + 1] > r2[2 * best + 1]))
				best = r;
		if (best < 0)
			break;
		hprintf("PROF pc %08x %u\n", r2[2 * best], r2[2 * best + 1]);
		r2[2 * best + 1] = 0;
		}
	hprintf("PROF total %d samples\n", n);
	}
