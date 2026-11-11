/* OpenGL ES 1.1 and 2.0: the forwarding runtime under the generated stubs (gles_stubs.c). Nothing about GL is
   decided here (docs/devices/nokia-n8-rm596/gles-host.md): the call goes to the host with the calling
   thread's id (its current context is the host EGL's to know); what a call needs from the caller's memory
   the host reads itself. */
#include "gles_int.h"

unsigned* khr_gl_call(int aOp, int aFn, int aWords)
	{
	if (!khr_host_on())			/* the channel to the host, opened on the first call */
		return 0;
	khr_lock();
	unsigned* p = khr_emit(aOp, 2 + aWords);
	if (!p)
		{
		khr_unlock();
		return 0;
		}
	p[0] = khr_thread_id();
	p[1] = (unsigned)aFn;
	return p + 2;
	}

const unsigned* khr_gl_sync(int aOutBytes)
	{
	return khr_flush(aOutBytes) == 0 ? khr_out() : 0;
	}

void khr_gl_done(void) { khr_unlock(); }

/* strings stay valid for the process (glGetString's contract): one copy of each */
const char* khr_gl_string(const unsigned* aOut)
	{
	static char* sKept[32];
	static int sN;
	const char* ret = 0;
	if (aOut && aOut[0] != ~0u && aOut[0] < 4092)		/* the transport's bound (4096-byte reply) */
		{
		unsigned len = aOut[0];
		const char* s = (const char*)(aOut + 1);
		for (int i = 0; i < sN && !ret; ++i)
			{
			unsigned k = 0;
			while (k < len && sKept[i][k] == s[k]) ++k;
			if (k == len && !sKept[i][len])
				ret = sKept[i];
			}
		if (!ret && sN < 32 && (sKept[sN] = (char*)khr_alloc((int)len + 1)) != 0)
			{
			for (unsigned k = 0; k < len; ++k)
				sKept[sN][k] = s[k];
			sKept[sN][len] = 0;
			ret = sKept[sN++];
			}
		}
	khr_gl_done();
	return ret;
	}

/* map: the host says how many bytes, the guest gives that memory, the host fills it; unmap: the host
   uploads it and says which memory to free */
unsigned khr_gl_map(int aOp, int aFn, unsigned* aArgs, int aMap)
	{
	if (!aMap)
		{
		const unsigned* r = khr_gl_sync(8);
		unsigned ok = r ? r[0] : 0, va = r ? r[1] : 0;
		khr_gl_done();
		if (va)
			khr_big_free((void*)va);
		return ok;
		}
	unsigned target = aArgs[0], access = aArgs[1];
	const unsigned* r = khr_gl_sync(4);
	unsigned size = r ? r[0] : 0;
	khr_gl_done();
	if (!size)
		return 0;
	void* d = khr_big_alloc((int)size);
	if (!d)
		return 0;
	unsigned* p = khr_gl_call(aOp, aFn, 3);
	if (!p) { khr_big_free(d); return 0; }
	p[0] = target; p[1] = access; p[2] = (unsigned)d;
	r = khr_gl_sync(4);
	unsigned ok = r ? r[0] : 0;
	khr_gl_done();
	if (!ok) { khr_big_free(d); return 0; }
	return (unsigned)d;
	}

/* eglGetProcAddress (egl2.inl): the stub of that name for the API the host said */
struct KhrGlProc { const char* name; void (*fn)(void); int api; };
extern const struct KhrGlProc khr_gl_procs[];

void* khr_gl_proc(const char* aName, int aApi)
	{
	for (const struct KhrGlProc* p = khr_gl_procs; p->name; ++p)
		{
		const char* a = p->name;
		const char* b = aName;
		while (*a && *a == *b) ++a, ++b;
		if (!*a && !*b && p->api == aApi)
			return (void*)p->fn;
		}
	return 0;
	}
