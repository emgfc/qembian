// eglprobe: what a phone's EGL / OpenVG / OpenGL ES report, written to a text file, so a real phone's
// answers (the oracle, docs/real-phone-tracing.md) can be compared with qembian's (src/khr).
// Started from the application menu (eglprobe_reg.rsc); writes E:\eglprobe\<n>.txt (C:\data\eglprobe\ if
// E: is missing; a new number per run, nothing overwritten) and shows a note with the path when done.
// Records: EGL version and strings, every config with every EGL 1.4 config attribute 0x3020..0x3042
// (value or the error), OpenVG strings and limits (vgGeti), a pbuffer from a VGImage (alf's effect
// buffers), OpenGL ES 1.1 strings. No capabilities needed.
// SPDX-License-Identifier: MIT
#include <e32base.h>
#include <f32file.h>

// ---- EGL / VG / GLES, declared here (the 9.2 SDK the agent is built with has no EGL 1.4 headers) ----
typedef int EGLint;
typedef unsigned int EGLBoolean, EGLenum;
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext, *EGLClientBuffer;
typedef unsigned int VGHandle;
typedef int VGint;
typedef unsigned int VGenum;
extern "C" {
EGLDisplay eglGetDisplay(int);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
EGLBoolean eglTerminate(EGLDisplay);
const char *eglQueryString(EGLDisplay, EGLint);
EGLBoolean eglGetConfigs(EGLDisplay, EGLConfig *, EGLint, EGLint *);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint *);
EGLint eglGetError(void);
EGLBoolean eglBindAPI(EGLenum);
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *);
EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig, const EGLint *);
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint, EGLint *);
const unsigned char *vgGetString(VGenum);
VGint vgGeti(VGenum);
VGenum vgGetError(void);
VGHandle vgCreateImage(VGenum, VGint, VGint, VGenum);
void vgDestroyImage(VGHandle);
const unsigned char *glGetString(unsigned int);
}
enum { EGL_SUCCESS = 0x3000, EGL_NONE = 0x3038, EGL_VENDOR = 0x3053, EGL_VERSION = 0x3054,
       EGL_EXTENSIONS = 0x3055, EGL_CLIENT_APIS = 0x308D, EGL_SURFACE_TYPE = 0x3033,
       EGL_RENDERABLE_TYPE = 0x3040, EGL_PBUFFER_BIT = 1, EGL_OPENVG_BIT = 2, EGL_OPENGL_ES_BIT = 1,
       EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056, EGL_OPENVG_API = 0x30A1, EGL_OPENGL_ES_API = 0x30A0,
       EGL_OPENVG_IMAGE = 0x3096, EGL_ALPHA_SIZE = 0x3021, EGL_RED_SIZE = 0x3024 };
enum { VG_VENDOR = 0x2300, VG_RENDERER, VG_VERSION, VG_EXTENSIONS, VG_sRGBA_8888_PRE = 2 };

static const struct { EGLint id; const char *name; } KAttr[] = {
	{0x3020,"BUFFER_SIZE"},{0x3021,"ALPHA_SIZE"},{0x3022,"BLUE_SIZE"},{0x3023,"GREEN_SIZE"},{0x3024,"RED_SIZE"},
	{0x3025,"DEPTH_SIZE"},{0x3026,"STENCIL_SIZE"},{0x3027,"CONFIG_CAVEAT"},{0x3028,"CONFIG_ID"},{0x3029,"LEVEL"},
	{0x302A,"MAX_PBUFFER_HEIGHT"},{0x302B,"MAX_PBUFFER_PIXELS"},{0x302C,"MAX_PBUFFER_WIDTH"},
	{0x302D,"NATIVE_RENDERABLE"},{0x302E,"NATIVE_VISUAL_ID"},{0x302F,"NATIVE_VISUAL_TYPE"},
	{0x3030,"PRESERVED_RESOURCES"},{0x3031,"SAMPLES"},{0x3032,"SAMPLE_BUFFERS"},{0x3033,"SURFACE_TYPE"},
	{0x3034,"TRANSPARENT_TYPE"},{0x3035,"TRANSPARENT_BLUE_VALUE"},{0x3036,"TRANSPARENT_GREEN_VALUE"},
	{0x3037,"TRANSPARENT_RED_VALUE"},{0x3039,"BIND_TO_TEXTURE_RGB"},{0x303A,"BIND_TO_TEXTURE_RGBA"},
	{0x303B,"MIN_SWAP_INTERVAL"},{0x303C,"MAX_SWAP_INTERVAL"},{0x303D,"LUMINANCE_SIZE"},{0x303E,"ALPHA_MASK_SIZE"},
	{0x303F,"COLOR_BUFFER_TYPE"},{0x3040,"RENDERABLE_TYPE"},{0x3041,"MATCH_NATIVE_PIXMAP"},{0x3042,"CONFORMANT"},
};
static const struct { VGenum id; const char *name; } KVgLimit[] = {
	{0x1160,"MAX_SCISSOR_RECTS"},{0x1161,"MAX_DASH_COUNT"},{0x1162,"MAX_KERNEL_SIZE"},
	{0x1163,"MAX_SEPARABLE_KERNEL_SIZE"},{0x1164,"MAX_COLOR_RAMP_STOPS"},{0x1165,"MAX_IMAGE_WIDTH"},
	{0x1166,"MAX_IMAGE_HEIGHT"},{0x1167,"MAX_IMAGE_PIXELS"},{0x1168,"MAX_IMAGE_BYTES"},{0x1169,"MAX_FLOAT"},
	{0x116A,"MAX_GAUSSIAN_STD_DEVIATION"},
};

// one output line at a time, built with appends (the 9.2 SDK's VA_LIST does not fit GCC 14's va_list)
class TOut {
public:
	TOut(RFile &aF) : iF(aF) {}
	TOut &S(const char *aS) { iB.Append(TPtrC8((const TUint8 *)aS, User::StringLength((const TUint8 *)aS))); return *this; }
	TOut &N(TInt aV) { iB.AppendNum(aV); return *this; }
	TOut &X(TUint aV) { iB.Append(_L8("0x")); iB.AppendNum(aV, EHex); return *this; }
	void E() { iB.Append('\n'); iF.Write(iB); iB.Zero(); }
	void Str(const char *aLabel, const void *aS) {	// a C string, written in pieces (extension lists are long)
		const char *s = (const char *)aS;
		if (!s) { S(aLabel).S(": (null, EGL error ").X(eglGetError()).S(")").E(); return; }
		S(aLabel).S(": "); iF.Write(iB); iB.Zero();
		iF.Write(TPtrC8((const TUint8 *)s, User::StringLength((const TUint8 *)s)));
		iF.Write(_L8("\n"));
	}
	RFile &iF;
	TBuf8<256> iB;
};

static TInt OpenOutput(RFs &aFs, RFile &aF, TFileName &aName)
{
	const TText *dirs[] = { _S("E:\\eglprobe\\"), _S("C:\\data\\eglprobe\\") };
	for (TUint d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		TPtrC dir(dirs[d]);
		TInt r = aFs.MkDirAll(dir);
		if (r != KErrNone && r != KErrAlreadyExists)
			continue;
		for (TInt n = 1; n < 1000; n++) {
			aName = dir;
			aName.AppendNum(n);
			aName.Append(_L(".txt"));
			r = aF.Create(aFs, aName, EFileWrite | EFileShareExclusive);
			if (r == KErrNone)
				return KErrNone;
			if (r != KErrAlreadyExists)
				break;
		}
	}
	return KErrNotFound;
}

static void Probe(TOut &o)
{
	o.S("eglprobe 1 (qembian src/n95apps/eglprobe)").E();
	EGLDisplay d = eglGetDisplay(0);
	EGLint maj = 0, min = 0;
	EGLBoolean ok = eglInitialize(d, &maj, &min);
	o.S("eglInitialize: ").N(ok).S(" error ").X(eglGetError()).S(" version ").N(maj).S(".").N(min).E();
	if (!ok)
		return;
	o.Str("EGL_VENDOR", eglQueryString(d, EGL_VENDOR));
	o.Str("EGL_VERSION", eglQueryString(d, EGL_VERSION));
	o.Str("EGL_CLIENT_APIS", eglQueryString(d, EGL_CLIENT_APIS));
	o.Str("EGL_EXTENSIONS", eglQueryString(d, EGL_EXTENSIONS));

	EGLConfig cfg[64];
	EGLint n = 0;
	ok = eglGetConfigs(d, cfg, 64, &n);
	o.S("eglGetConfigs: ").N(ok).S(", ").N(n).S(" configs").E();
	for (EGLint i = 0; i < n; i++) {
		o.S("config ").N(i).S(":").E();
		for (TUint a = 0; a < sizeof(KAttr) / sizeof(KAttr[0]); a++) {
			EGLint v = -12345;
			EGLBoolean r = eglGetConfigAttrib(d, cfg[i], KAttr[a].id, &v);
			if (r)
				o.S("  ").S(KAttr[a].name).S(" (").X(KAttr[a].id).S(") = ").N(v).S(" (").X(v).S(")").E();
			else
				o.S("  ").S(KAttr[a].name).S(" (").X(KAttr[a].id).S(") error ").X(eglGetError()).E();
		}
	}

	// OpenVG: a pbuffer context, strings, limits, a pbuffer from a VGImage
	const EGLint vgAttr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENVG_BIT,
	                          EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig vc = 0;
	EGLint vn = 0;
	ok = eglChooseConfig(d, vgAttr, &vc, 1, &vn);
	o.S("OpenVG: eglChooseConfig ").N(ok).S(" (").N(vn).S(" configs, error ").X(eglGetError()).S(")").E();
	if (ok && vn > 0) {
		eglBindAPI(EGL_OPENVG_API);
		const EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
		EGLSurface s = eglCreatePbufferSurface(d, vc, pb);
		EGLContext c = eglCreateContext(d, vc, 0, 0);
		ok = eglMakeCurrent(d, s, s, c);
		o.S("OpenVG: pbuffer ").X((TUint)s).S(" context ").X((TUint)c).S(" current ").N(ok).S(" (error ").X(eglGetError()).S(")").E();
		if (ok) {
			o.Str("VG_VENDOR", vgGetString(VG_VENDOR));
			o.Str("VG_RENDERER", vgGetString(VG_RENDERER));
			o.Str("VG_VERSION", vgGetString(VG_VERSION));
			o.Str("VG_EXTENSIONS", vgGetString(VG_EXTENSIONS));
			for (TUint k = 0; k < sizeof(KVgLimit) / sizeof(KVgLimit[0]); k++)
				o.S("  VG_").S(KVgLimit[k].name).S(" = ").N(vgGeti(KVgLimit[k].id)).E();
			VGHandle img = vgCreateImage(VG_sRGBA_8888_PRE, 128, 64, 7 /* nonantialiased | faster | better */);
			o.S("vgCreateImage 128x64 sRGBA_8888_PRE: ").X(img).S(" (vg error ").X(vgGetError()).S(")").E();
			if (img) {
				EGLSurface is = eglCreatePbufferFromClientBuffer(d, EGL_OPENVG_IMAGE, (EGLClientBuffer)img, vc, 0);
				EGLint e = eglGetError(), w = 0, h = 0;
				o.S("eglCreatePbufferFromClientBuffer(VGImage): ").X((TUint)is).S(" (error ").X(e).S(")").E();
				if (is) {
					eglQuerySurface(d, is, EGL_WIDTH, &w);
					eglQuerySurface(d, is, EGL_HEIGHT, &h);
					ok = eglMakeCurrent(d, is, is, c);
					o.S("  its size ").N(w).S("x").N(h).S(", current ").N(ok).S(" (error ").X(eglGetError()).S(")").E();
					eglMakeCurrent(d, s, s, c);
					eglDestroySurface(d, is);
				}
				vgDestroyImage(img);
			}
		}
		eglMakeCurrent(d, 0, 0, 0);
		if (c) eglDestroyContext(d, c);
		if (s) eglDestroySurface(d, s);
	}

	// OpenGL ES 1.1
	const EGLint glAttr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_NONE };
	EGLConfig gc = 0;
	EGLint gn = 0;
	ok = eglChooseConfig(d, glAttr, &gc, 1, &gn);
	o.S("OpenGL ES: eglChooseConfig ").N(ok).S(" (").N(gn).S(" configs, error ").X(eglGetError()).S(")").E();
	if (ok && gn > 0) {
		eglBindAPI(EGL_OPENGL_ES_API);
		const EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
		EGLSurface s = eglCreatePbufferSurface(d, gc, pb);
		EGLContext c = eglCreateContext(d, gc, 0, 0);
		ok = eglMakeCurrent(d, s, s, c);
		o.S("OpenGL ES: current ").N(ok).S(" (error ").X(eglGetError()).S(")").E();
		if (ok) {
			o.Str("GL_VENDOR", glGetString(0x1F00));
			o.Str("GL_RENDERER", glGetString(0x1F01));
			o.Str("GL_VERSION", glGetString(0x1F02));
			o.Str("GL_EXTENSIONS", glGetString(0x1F03));
		}
		eglMakeCurrent(d, 0, 0, 0);
		if (c) eglDestroyContext(d, c);
		if (s) eglDestroySurface(d, s);
	}
	eglTerminate(d);
	o.S("end").E();
}

GLDEF_C TInt E32Main()
{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return KErrGeneral;
	RFile f;
	TFileName name;
	TInt r = OpenOutput(fs, f, name);
	if (r == KErrNone) {
		TOut o(f);
		Probe(o);
		f.Close();
	}
	RNotifier nt;
	if (nt.Connect() == KErrNone) {
		TInt button = 0;
		TRequestStatus st;
		TBuf<80> l1(r == KErrNone ? _L("eglprobe: written") : _L("eglprobe: no output file"));
		nt.Notify(l1, name, _L("OK"), KNullDesC, button, st);
		User::WaitForRequest(st);
		nt.Close();
	}
	fs.Close();
	return r;
}

// ---- process start-up for devagent/startup.s (as oracle.cpp): heap, C++ runtime, E32Main ----
struct SDaThreadCreateInfo
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
IMPORT_C TInt DaSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");
IMPORT_C void DaExceptionsGlobals(TAny* aThis) asm("_ZN23TCppRTExceptionsGlobalsC1Ev");

extern "C" void DaRunThread(TBool aNotFirst, SDaThreadCreateInfo& aInfo)
	{
	TUint64 exceptionGlobals[64];
	TInt r = DaSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
	if (r == KErrNone)
		{
		DaExceptionsGlobals(exceptionGlobals);		// after the heap: its Dll::SetTls allocates
		if (aNotFirst)
			r = (*aInfo.iFunction)(aInfo.iPtr);
		else
			{
			User::InitProcess();
			r = E32Main();
			}
		}
	User::Exit(r);
	}

extern "C" void DaInvariant() { User::Invariant(); }
extern "C" void DaHandleException(TAny* aInfo) { User::HandleException(aInfo); }
