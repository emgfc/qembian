// playfile.exe: plays a sound through the phone's own MMF stack, for audio tests (syborg-snd, the DSP stand-ins).
//   sydb run 'c:\sys\bin\playfile.exe' 'c:\data\test.mp3'   a file (WAV, MP3, AAC, ... whatever the phone's
//                                                            controllers take), CMdaAudioPlayerUtility
//   sydb run 'c:\sys\bin\playfile.exe' tone [HZ] [MS]       a tone, CMdaAudioToneUtility (ARM tone synthesis)
// Output on RDebug: "PLAYFILE: open 0", "PLAYFILE: play", "PLAYFILE: done E (T ms)": E = the player's completion
// error, T = wall time from play to completion (compare with the clip's length).
#include <e32base.h>
#include <e32debug.h>
#include <mdaaudiosampleplayer.h>
#include <mdaaudiotoneplayer.h>

static TTime Started;

static TInt Ms()
	{
	TTime now;
	now.UniversalTime();
	return (TInt)(now.MicroSecondsFrom(Started).Int64() / 1000);
	}

class CPlayer : public CBase, public MMdaAudioPlayerCallback, public MMdaAudioToneObserver
	{
public:
	TInt iErr;
	CMdaAudioPlayerUtility* iFile;
	CMdaAudioToneUtility* iTone;
	TInt iHz, iMs;
	~CPlayer() { delete iFile; delete iTone; }
	void MapcInitComplete(TInt aError, const TTimeIntervalMicroSeconds& aDuration)
		{
		RDebug::Printf("PLAYFILE: open %d, duration %d ms", aError, (TInt)(aDuration.Int64() / 1000));
		if (aError) { iErr = aError; CActiveScheduler::Stop(); return; }
		iFile->SetVolume(iFile->MaxVolume() / 2);
		Started.UniversalTime();
		RDebug::Printf("PLAYFILE: play");
		iFile->Play();
		}
	void MapcPlayComplete(TInt aError)
		{
		RDebug::Printf("PLAYFILE: done %d (%d ms)", aError, Ms());
		iErr = aError;
		CActiveScheduler::Stop();
		}
	void MatoPrepareComplete(TInt aError)
		{
		RDebug::Printf("PLAYFILE: tone prepared %d", aError);
		if (aError) { iErr = aError; CActiveScheduler::Stop(); return; }
		iTone->SetVolume(iTone->MaxVolume() / 2);
		Started.UniversalTime();
		RDebug::Printf("PLAYFILE: play");
		iTone->Play();
		}
	void MatoPlayComplete(TInt aError)
		{
		RDebug::Printf("PLAYFILE: done %d (%d ms)", aError, Ms());
		iErr = aError;
		CActiveScheduler::Stop();
		}
	};

static TInt Arg(TLex& aLex, TInt aDefault)
	{
	TInt v;
	aLex.SkipSpace();
	return aLex.Val(v) == KErrNone ? v : aDefault;
	}

static void RunL()
	{
	TBuf<256> cmd;
	User::CommandLine(cmd);
	TLex lex(cmd);
	TPtrC first = lex.NextToken();
	CPlayer* p = new (ELeave) CPlayer;
	CleanupStack::PushL(p);
	if (first.CompareF(_L("tone")) == 0)
		{
		p->iHz = Arg(lex, 1000);
		p->iMs = Arg(lex, 500);
		p->iTone = CMdaAudioToneUtility::NewL(*p);
		p->iTone->PrepareToPlayTone(p->iHz, TTimeIntervalMicroSeconds(p->iMs * 1000));
		}
	else
		p->iFile = CMdaAudioPlayerUtility::NewFilePlayerL(first, *p);
	CActiveScheduler::Start();
	RDebug::Printf("PLAYFILE: exit %d", p->iErr);
	CleanupStack::PopAndDestroy(p);
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* tc = CTrapCleanup::New();
	CActiveScheduler* as = new CActiveScheduler;
	CActiveScheduler::Install(as);
	TRAPD(err, RunL());
	if (err)
		RDebug::Printf("PLAYFILE: leave %d", err);
	delete as;
	delete tc;
	return err;
	}

// ---- process start-up for devagent/startup.s (as dafs.cpp): heap, C++ exception globals, E32Main ----
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
		DaExceptionsGlobals(exceptionGlobals);
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
