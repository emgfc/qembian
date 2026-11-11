#include <e32base.h>
#include <e32debug.h>
#include <sensrvchannelfinder.h>
#include <sensrvchannel.h>
#include <sensrvchannelinfo.h>
class RCF: public RSessionBase { public: TInt Open(){return CreateSession(_L("CFServer"),TVersion(1,0,1),2);} TInt Query(const TDesC8& q,TDes8& out,TDes8& size){return SendReceive(9,TIpcArgs(&out,&q,&size));}};
static void Word(TDes8& b,TInt n){ b.Append(n&255);b.Append((n>>8)&255); }
static void Str(TDes8& b,const TDesC& s){ Word(b,s.Length()); for(TInt i=0;i<s.Length();i++)Word(b,s[i]); }
static TInt W(const TDesC8& b,TInt& off){if(off+2>b.Length())return -1; TInt r=b[off]|b[off+1]<<8;off+=2;return r;}
static TBool S(const TDesC8& b,TInt& off,TDes& s){TInt n=W(b,off);if(n<0||n>s.MaxLength()||off+2*n>b.Length())return EFalse;s.Zero();while(n--)s.Append(W(b,off));return ETrue;}
static void Query(RCF& c,const TDesC& source,const TDesC& type){TBuf8<512> q;Str(q,source);Str(q,type);Word(q,0); HBufC8* h=HBufC8::New(32768);if(!h)return;TPtr8 out=h->Des();TPckgBuf<TInt> size(32768);TInt r=c.Query(q,out,size);RDebug::Print(_L("CFPROBE: %S:%S r=%d bytes=%d needed=%d"),&source,&type,r,out.Length(),size());if(r==0&&out.Length()>1){TInt off=0,n=W(out,off);RDebug::Printf("CFPROBE: count=%d",n);for(TInt i=0;i<n;i++){TBuf<256> s,t,v;if(!S(out,off,s)||!S(out,off,t)||!S(out,off,v)||off+10>out.Length()){RDebug::Printf("CFPROBE: malformed at %d",off);break;}off+=10;RDebug::Print(_L("CFPROBE: %S:%S = %S"),&s,&t,&v);}}delete h;}
static void SensorsL(){CSensrvChannelFinder* f=CSensrvChannelFinder::NewLC();RSensrvChannelInfoList list;CleanupClosePushL(list);TSensrvChannelInfo q;f->FindChannelsL(list,q);RDebug::Printf("CFPROBE: sensors=%d",list.Count());for(TInt i=0;i<list.Count();i++){const TSensrvChannelInfo& c=list[i];RDebug::Printf("CFPROBE: sensor id=%d type=%08x qty=%d size=%d",c.iChannelId,c.iChannelType,c.iQuantity,c.iDataItemSize);}for(TInt i=0;i<list.Count();i++){if(list[i].iChannelType!=0x10205088&&list[i].iChannelType!=0x1020507e)continue;class RS:public RSessionBase{public:TInt Open(){return CreateSession(_L("!SensorServer"),TVersion(1,0,0),2);}TInt Channel(TInt id,TDes8& count,TDes8& size){return SendReceive(1,TIpcArgs(id,&count,&size));}} sess;TInt r=sess.Open();TPckgBuf<TInt> n(0),sz(0);if(!r)r=sess.Channel(list[i].iChannelId,n,sz);RDebug::Printf("CFPROBE: raw open type=%08x r=%d count=%d size=%d",list[i].iChannelType,r,n(),sz());sess.Close();}CleanupStack::PopAndDestroy(2,f);}
GLDEF_C TInt E32Main(){CTrapCleanup* cleanup=CTrapCleanup::New();CActiveScheduler* scheduler=new CActiveScheduler; CActiveScheduler::Install(scheduler); RCF c;TInt r=c.Open();RDebug::Printf("CFPROBE: connect=%d",r);if(!r){Query(c,_L("ContextFramework"),_L("Status"));Query(c,_L("Sensor"),_L("Setting.Active"));Query(c,_L("Sensor"),_L("Setting.TurningInteraction.AutoRotation"));Query(c,_L("Device"),_L("Display.Status.Light"));Query(c,_L("Device"),_L("DeviceLock.Status"));Query(c,_L("System"),_L("Keyguard.Status"));Query(c,_L("Device"),_L("Orientation.Active"));Query(c,_L("Sensor"),_L("Event.Orientation"));}c.Close();TTrapHandler* trap=User::MarkCleanupStack(); SensorsL(); User::UnMarkCleanupStack(trap); TInt err=0;RDebug::Printf("CFPROBE: sensors result=%d",err);delete cleanup;return r;}
struct SDaThreadCreateInfo
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
IMPORT_C TInt DaSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");

// TCppRTExceptionsGlobals (drtaeabi): the thread's C++ exception globals and emergency buffer, which the
// real EXE startup (CallThrdProcEntry) keeps on each thread's entry stack frame; it registers itself in
// drtaeabi's TLS. Without it the first leave in any thread of this process (also one that library code
// traps itself) dies in __cxa_allocate_exception (KERN-EXEC 3). ~0x90 bytes; room for more.
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
