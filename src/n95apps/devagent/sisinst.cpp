// sisinst: installs a .sis/.sisx without the phone's installer UI, answering and logging every question
// the Software Install server asks (`sydb sis install`, docs/sydb.md).
//
//   sisinst.exe PACKAGE [drive=E] [lang=1] [log=PATH]
//
// It calls Swi::Launcher::Install (sishelper.dll, the native installer API under the S60 installer UI) with
// its own MUiHandler. Every dialog the UI would show arrives here: install?, capabilities, language, drive,
// upgrade, options, text/licence, security warning, OCSP result, missing dependency, applications in use,
// questions (incompatible package, overwrite), errors. Each one is logged with its parameters and answered
// "yes" (drive: the requested one, else the one with the most free space; language: the requested one,
// else English, else the first; options: all). Lines go to RDebug ("SISINST: ...", the serial log) and to
// the log file (default: PACKAGE + ".log"), flushed per line. Exit value = Install's result (0 or a
// system/SWI error code, e.g. -10254 security error, -10149 not enough space, -3 a dialog said no).
//
// Launcher::Install needs TrustedUI (this exe: All-TCB). The interface below mirrors the Symbian Foundation
// appinstall package (installationservices/swi/inc/swi/msisuihandlers.h and launcher.h, EPL): only the
// layout matters (virtual order, argument types); the types we only pass on are declared, not defined.
// sishelper.dll exports (legacy, pre-USIF DEF; Belle 111.040 has the same 91 exports): CInstallPrefs::NewL
// @3, Launcher::Install(MUiHandler&, const TFileName&, const CInstallPrefs&) @16 -> sishelper.def.
#include <e32base.h>
#include <e32debug.h>
#include <f32file.h>

class CApaMaskedBitmap;
class CPKIXValidationResultBase;
class TOCSPOutcome;

namespace Swi
{
class CCertificateInfo;
class MCancelHandler;

// layout of Swi::CAppInfo (vptr, iAppName, iAppVendor, iAppVersion); its accessors are inline
class CAppInfo : public CBase
	{
public:
	const TDesC& AppName() const { return iAppName ? (const TDesC&)*iAppName : KNullDesC(); }
	const TDesC& AppVendor() const { return iAppVendor ? (const TDesC&)*iAppVendor : KNullDesC(); }
	const TVersion& AppVersion() const { return iAppVersion; }
private:
	CAppInfo();
	HBufC* iAppName;
	HBufC* iAppVendor;
	TVersion iAppVersion;
	};

enum TErrorDialog { EUiAlreadyInRom };
enum TFileTextOption { EInstFileTextOptionContinue };
enum TInstallEvent { EEventSetProgressBarFinalValue, EEventUpdateProgressBar };
enum TInstallCancellableEvent { EEventRemovingFiles };
enum TQuestionDialog { EQuestionIncompatible };
enum TSignatureValidationResult { EValidationSucceeded };
enum TRevocationDialogMessage { EInvalidRevocationServerUrl };

class MCommonDialogs
	{
public:
	virtual TBool DisplayTextL(const CAppInfo& aAppInfo, TFileTextOption aOption, const TDesC& aText) = 0;
	virtual void DisplayErrorL(const CAppInfo& aAppInfo, TErrorDialog aType, const TDesC& aParam) = 0;
	virtual TBool DisplayDependencyBreakL(const CAppInfo& aAppInfo, const RPointerArray<TDesC>& aComponents) = 0;
	virtual TBool DisplayApplicationsInUseL(const CAppInfo& aAppInfo, const RPointerArray<TDesC>& aAppNames) = 0;
	virtual TBool DisplayQuestionL(const CAppInfo& aAppInfo, TQuestionDialog aQuestion, const TDesC& aDes) = 0;
	};

class MInstallerUiHandler
	{
public:
	virtual TBool DisplayInstallL(const CAppInfo& aAppInfo, const CApaMaskedBitmap* aLogo,
		const RPointerArray<CCertificateInfo>& aCertificates) = 0;
	virtual TBool DisplayGrantCapabilitiesL(const CAppInfo& aAppInfo, const TCapabilitySet& aCapabilitySet) = 0;
	virtual TInt DisplayLanguageL(const CAppInfo& aAppInfo, const RArray<TLanguage>& aLanguages) = 0;
	virtual TInt DisplayDriveL(const CAppInfo& aAppInfo, TInt64 aSize, const RArray<TChar>& aDriveLetters,
		const RArray<TInt64>& aDriveSpaces) = 0;
	virtual TBool DisplayUpgradeL(const CAppInfo& aAppInfo, const CAppInfo& aExistingAppInfo) = 0;
	virtual TBool DisplayOptionsL(const CAppInfo& aAppInfo, const RPointerArray<TDesC>& aOptions,
		RArray<TBool>& aSelections) = 0;
	virtual TBool HandleInstallEventL(const CAppInfo& aAppInfo, TInstallEvent aEvent, TInt aValue,
		const TDesC& aDes) = 0;
	virtual void HandleCancellableInstallEventL(const CAppInfo& aAppInfo, TInstallCancellableEvent aEvent,
		MCancelHandler& aCancelHandler, TInt aValue, const TDesC& aDes) = 0;
	virtual TBool DisplaySecurityWarningL(const CAppInfo& aAppInfo, TSignatureValidationResult aResult,
		RPointerArray<CPKIXValidationResultBase>& aPkixResults, RPointerArray<CCertificateInfo>& aCertificates,
		TBool aInstallAnyway) = 0;
	virtual TBool DisplayOcspResultL(const CAppInfo& aAppInfo, TRevocationDialogMessage aMessage,
		RPointerArray<TOCSPOutcome>& aOutcomes, RPointerArray<CCertificateInfo>& aCertificates,
		TBool aWarningOnly) = 0;
	virtual void DisplayCannotOverwriteFileL(const CAppInfo& aAppInfo, const CAppInfo& aInstalledAppInfo,
		const TDesC& aFileName) = 0;
	virtual TBool DisplayMissingDependencyL(const CAppInfo& aAppInfo, const TDesC& aDependencyName,
		TVersion aWantedVersionFrom, TVersion aWantedVersionTo, TVersion aInstalledVersion) = 0;
	};

class MUninstallerUiHandler
	{
public:
	virtual TBool DisplayUninstallL(const CAppInfo& aAppInfo) = 0;
	};

class MUiHandler : public MCommonDialogs, public MInstallerUiHandler, public MUninstallerUiHandler
	{
	};

class CInstallPrefs : public CBase
	{
public:
	static IMPORT_C CInstallPrefs* NewL();		// sishelper @3; no revocation (OCSP) check by default
	};

class Launcher
	{
public:
	static IMPORT_C TInt Install(MUiHandler& aUiHandler, const TFileName& aFileName,
		const CInstallPrefs& aInstallPrefs);	// sishelper @16
	};
}	// namespace Swi

using namespace Swi;

// ---- log ----

static RFile LogFile;
static TBool HaveLog;

// a macro, not a variadic function (the SDK's VA_LIST does not build with GCC 14); arguments must fit 600
#define Log(...) do { TBuf<600> _lb; _lb.Format(__VA_ARGS__); LogLine(_lb); } while (0)

static void LogLine(const TDesC& b)
	{
	RDebug::Print(_L("SISINST: %S"), &b);
	if (HaveLog)
		{
		TBuf8<620> l;
		for (TInt i = 0; i < b.Length(); ++i)
			l.Append(b[i] < 0x80 ? (TUint8)b[i] : (TUint8)'?');	// the file is ASCII; RDebug has the rest
		l.Append('\n');
		LogFile.Write(l);
		LogFile.Flush();
		}
	}

// long text (licence, notes): in pieces of 200 characters, line breaks shown as '|'
static void LogText(const TDesC& aPrefix, const TDesC& aText)
	{
	TBuf<200> part;
	for (TInt at = 0; at < aText.Length(); at += part.MaxLength())
		{
		part = aText.Mid(at, Min(part.MaxLength(), aText.Length() - at));
		for (TInt i = 0; i < part.Length(); ++i)
			if (part[i] == '\n' || part[i] == '\r' || part[i] == 0x2029)
				part[i] = '|';
		Log(_L("%S %S"), &aPrefix, &part);
		}
	}

static void LogApp(const TDesC& aWhat, const CAppInfo& aApp)
	{
	const TVersion& v = aApp.AppVersion();
	Log(_L("%S: \"%S\" by \"%S\" %d.%d.%d"), &aWhat, &aApp.AppName(), &aApp.AppVendor(),
		v.iMajor, v.iMinor, v.iBuild);
	}

static void LogList(const TDesC& aWhat, const RPointerArray<TDesC>& aList)
	{
	Log(_L("%S: %d"), &aWhat, aList.Count());
	for (TInt i = 0; i < aList.Count(); ++i)
		Log(_L("  [%d] %S"), i, aList[i]);
	}

static const TText* const KCapNames[] =
	{
	_S("TCB"), _S("CommDD"), _S("PowerMgmt"), _S("MultimediaDD"), _S("ReadDeviceData"), _S("WriteDeviceData"),
	_S("DRM"), _S("TrustedUI"), _S("ProtServ"), _S("DiskAdmin"), _S("NetworkControl"), _S("AllFiles"),
	_S("SwEvent"), _S("NetworkServices"), _S("LocalServices"), _S("ReadUserData"), _S("WriteUserData"),
	_S("Location"), _S("SurroundingsDD"), _S("UserEnvironment")
	};

static const TText* const KErrorNames[] =
	{
	_S("AlreadyInRom"), _S("MissingDependency"), _S("RequireVer"), _S("RequireVerOrGreater"), _S("FileCorrupt"),
	_S("DiskNotPresent"), _S("CannotRead"), _S("CannotDelete"), _S("InvalidFileName"), _S("FileNotFound"),
	_S("InsufficientSpaceOnDrive"), _S("CapabilitiesCannotBeGranted"), _S("UnknownFile"), _S("MissingBasePackage"),
	_S("ConstraintsExceeded"), _S("SIDViolation"), _S("VIDViolation"), _S("NoMemoryInDrive"),
	_S("UIDPackageViolation"), _S("OSExeViolation"), _S("SIDMismatch"), _S("BlockingEclipsingFile")
	};

static const TText* const KSigNames[] =
	{
	_S("Succeeded"), _S("SelfSigned"), _S("NoCertificate"), _S("CertificateValidationError"), _S("NotPresent"),
	_S("CouldNotBeValidated"), _S("NoCodeSigningExtension"), _S("NoSupportedPolicyExtension"),
	_S("MandatorySignatureMissing")
	};

static const TText* const KEventNames[] =
	{
	_S("SetProgressBarFinalValue"), _S("UpdateProgressBar"), _S("AbortedInstall"), _S("CompletedInstall"),
	_S("CompletedUnInstall"), _S("AbortedUnInstall"), _S("CompletedNoMessage"), _S("Leave"), _S("OcspCheckEnd"),
	_S("DevCert")
	};

static const TText* const KCancellableNames[] =
	{ _S("RemovingFiles"), _S("ShuttingDownApps"), _S("CopyingFiles"), _S("OcspCheckStart") };

static const TText* const KTextOptions[] =
	{ _S("Continue"), _S("SkipOneIfNo"), _S("AbortIfNo"), _S("ExitIfNo"), _S("ForceAbort") };

#define NAME(table, i) TPtrC((TUint)(i) < sizeof(table) / sizeof(table[0]) ? table[i] : _S("?"))

// ---- the handler: every answer is "yes" ----

class CHandler : public CBase, public MUiHandler
	{
public:
	TChar iDrive;			// wanted drive (0 = most free space)
	TInt iLang;				// wanted language (-1 = English, else the first)
	TInt iFinal, iProgress, iLastPct;

	TBool DisplayTextL(const CAppInfo& aApp, TFileTextOption aOption, const TDesC& aText)
		{
		LogApp(_L("text"), aApp);
		TPtrC o(NAME(KTextOptions, aOption));
		Log(_L("text option %S, %d chars -> yes"), &o, aText.Length());
		LogText(_L("  |"), aText);
		return ETrue;
		}
	void DisplayErrorL(const CAppInfo& aApp, TErrorDialog aType, const TDesC& aParam)
		{
		LogApp(_L("error"), aApp);
		TPtrC n(NAME(KErrorNames, aType));
		Log(_L("error dialog %d %S param \"%S\""), aType, &n, &aParam);
		}
	TBool DisplayDependencyBreakL(const CAppInfo& aApp, const RPointerArray<TDesC>& aComponents)
		{
		LogApp(_L("dependency break"), aApp);
		LogList(_L("dependency break, components (-> continue)"), aComponents);
		return ETrue;
		}
	TBool DisplayApplicationsInUseL(const CAppInfo& aApp, const RPointerArray<TDesC>& aAppNames)
		{
		LogApp(_L("applications in use"), aApp);
		LogList(_L("applications in use (-> close them)"), aAppNames);
		return ETrue;
		}
	TBool DisplayQuestionL(const CAppInfo& aApp, TQuestionDialog aQuestion, const TDesC& aDes)
		{
		LogApp(_L("question"), aApp);
		TPtrC q(aQuestion == 0 ? _S("Incompatible") : aQuestion == 1 ? _S("OverwriteFile") : _S("?"));
		Log(_L("question %S \"%S\" -> yes"), &q, &aDes);
		return ETrue;
		}
	TBool DisplayInstallL(const CAppInfo& aApp, const CApaMaskedBitmap* aLogo,
		const RPointerArray<CCertificateInfo>& aCertificates)
		{
		LogApp(_L("install?"), aApp);
		Log(_L("install? logo %d, %d certificates -> yes"), aLogo != NULL, aCertificates.Count());
		return ETrue;
		}
	TBool DisplayGrantCapabilitiesL(const CAppInfo& aApp, const TCapabilitySet& aCaps)
		{
		LogApp(_L("grant capabilities"), aApp);
		TBuf<400> b;
		for (TInt c = 0; c < (TInt)(sizeof(KCapNames) / sizeof(KCapNames[0])); ++c)
			if (aCaps.HasCapability((TCapability)c))
				{
				b.Append(TPtrC(KCapNames[c]));
				b.Append(' ');
				}
		Log(_L("grant capabilities: %S-> yes"), &b);
		return ETrue;
		}
	TInt DisplayLanguageL(const CAppInfo& aApp, const RArray<TLanguage>& aLanguages)
		{
		LogApp(_L("language"), aApp);
		TBuf<300> b;
		TInt pick = 0;
		for (TInt i = 0; i < aLanguages.Count(); ++i)
			{
			if (b.Length() < b.MaxLength() - 12)
				b.AppendFormat(_L("%d "), aLanguages[i]);
			if (iLang >= 0 && aLanguages[i] == iLang)
				pick = i;
			}
		if (iLang < 0)
			for (TInt i = aLanguages.Count() - 1; i >= 0; --i)
				if (aLanguages[i] == ELangEnglish)
					pick = i;
		Log(_L("language: %S-> [%d] %d"), &b, pick, aLanguages.Count() ? aLanguages[pick] : -1);
		return pick;
		}
	TInt DisplayDriveL(const CAppInfo& aApp, TInt64 aSize, const RArray<TChar>& aDrives,
		const RArray<TInt64>& aSpaces)
		{
		LogApp(_L("drive"), aApp);
		Log(_L("drive: package needs %Ld bytes"), aSize);
		TInt pick = -1, most = -1;
		for (TInt i = 0; i < aDrives.Count(); ++i)
			{
			TInt64 space = i < aSpaces.Count() ? aSpaces[i] : 0;
			Log(_L("  [%d] %c: %Ld free"), i, (TUint)aDrives[i], space);
			if (TChar(aDrives[i]).GetUpperCase() == iDrive && space >= aSize)
				pick = i;
			if (most < 0 || space > aSpaces[most])
				most = i;
			}
		if (pick < 0)
			pick = most < 0 ? 0 : most;
		Log(_L("drive -> [%d] %c:"), pick, pick < aDrives.Count() ? (TUint)aDrives[pick] : '?');
		return pick;
		}
	TBool DisplayUpgradeL(const CAppInfo& aApp, const CAppInfo& aExisting)
		{
		LogApp(_L("upgrade, new"), aApp);
		LogApp(_L("upgrade, installed (-> yes)"), aExisting);
		return ETrue;
		}
	TBool DisplayOptionsL(const CAppInfo& aApp, const RPointerArray<TDesC>& aOptions, RArray<TBool>& aSelections)
		{
		LogApp(_L("options"), aApp);
		LogList(_L("options (-> all selected)"), aOptions);
		for (TInt i = 0; i < aSelections.Count(); ++i)
			aSelections[i] = ETrue;
		return ETrue;
		}
	TBool HandleInstallEventL(const CAppInfo& aApp, TInstallEvent aEvent, TInt aValue, const TDesC& aDes)
		{
		if (aEvent == EEventSetProgressBarFinalValue)
			iFinal = aValue;
		if (aEvent == EEventUpdateProgressBar)
			{
			iProgress += aValue;
			TInt pct = iFinal > 0 ? (TInt)((TInt64)iProgress * 100 / iFinal) : 0;
			if (pct / 10 == iLastPct / 10)
				return ETrue;				// progress: one line per 10%
			iLastPct = pct;
			Log(_L("progress %d%%"), pct);
			return ETrue;
			}
		TPtrC n(NAME(KEventNames, aEvent));
		Log(_L("event %S value %d \"%S\" (%S)"), &n, aValue, &aDes, &aApp.AppName());
		return ETrue;
		}
	void HandleCancellableInstallEventL(const CAppInfo& aApp, TInstallCancellableEvent aEvent,
		MCancelHandler& /*aCancel*/, TInt aValue, const TDesC& aDes)
		{
		TPtrC n(NAME(KCancellableNames, aEvent));
		Log(_L("event %S value %d \"%S\" (%S)"), &n, aValue, &aDes, &aApp.AppName());
		}
	TBool DisplaySecurityWarningL(const CAppInfo& aApp, TSignatureValidationResult aResult,
		RPointerArray<CPKIXValidationResultBase>& aPkix, RPointerArray<CCertificateInfo>& aCerts,
		TBool aInstallAnyway)
		{
		LogApp(_L("security warning"), aApp);
		TPtrC n(NAME(KSigNames, aResult));
		Log(_L("security warning: signature %S, %d pkix results, %d certificates, install anyway %d -> yes"),
			&n, aPkix.Count(), aCerts.Count(), aInstallAnyway);
		return ETrue;
		}
	TBool DisplayOcspResultL(const CAppInfo& aApp, TRevocationDialogMessage aMessage,
		RPointerArray<TOCSPOutcome>& aOutcomes, RPointerArray<CCertificateInfo>& aCerts, TBool aWarningOnly)
		{
		LogApp(_L("ocsp result"), aApp);
		Log(_L("ocsp result: message %d, %d outcomes, %d certificates, warning only %d -> yes"), aMessage,
			aOutcomes.Count(), aCerts.Count(), aWarningOnly);
		return ETrue;
		}
	void DisplayCannotOverwriteFileL(const CAppInfo& aApp, const CAppInfo& aInstalled, const TDesC& aFile)
		{
		LogApp(_L("cannot overwrite, new"), aApp);
		LogApp(_L("cannot overwrite, owner"), aInstalled);
		Log(_L("cannot overwrite file %S"), &aFile);
		}
	TBool DisplayMissingDependencyL(const CAppInfo& aApp, const TDesC& aName, TVersion aFrom, TVersion aTo,
		TVersion aInstalled)
		{
		LogApp(_L("missing dependency"), aApp);
		Log(_L("missing dependency \"%S\" wanted %d.%d.%d..%d.%d.%d installed %d.%d.%d -> continue"), &aName,
			aFrom.iMajor, aFrom.iMinor, aFrom.iBuild, aTo.iMajor, aTo.iMinor, aTo.iBuild,
			aInstalled.iMajor, aInstalled.iMinor, aInstalled.iBuild);
		return ETrue;
		}
	TBool DisplayUninstallL(const CAppInfo& aApp)
		{
		LogApp(_L("uninstall? (-> yes)"), aApp);
		return ETrue;
		}
	};

// ---- main ----

static TInt Run()
	{
	HBufC* cl = HBufC::New(User::CommandLineLength() + 1);
	if (!cl)
		return KErrNoMemory;
	TPtr clp = cl->Des();
	User::CommandLine(clp);
	TFileName pkg, logName;
	TChar drive('E');
	TInt lang = -1;
	TLex lx(clp);
	for (TPtrC tok = lx.NextToken(); tok.Length(); tok.Set(lx.NextToken()))
		{
		if (tok.Left(6).CompareF(_L("drive=")) == 0 && tok.Length() > 6)
			drive = TChar(tok[6]).GetUpperCase();
		else if (tok.Left(5).CompareF(_L("lang=")) == 0)
			TLex(tok.Mid(5)).Val(lang);
		else if (tok.Left(4).CompareF(_L("log=")) == 0)
			logName = tok.Mid(4).Left(logName.MaxLength());
		else if (!pkg.Length())
			pkg = tok.Left(pkg.MaxLength());
		}
	delete cl;
	if (!pkg.Length())
		{
		RDebug::Print(_L("SISINST: usage: sisinst.exe PACKAGE [drive=E] [lang=N] [log=PATH]"));
		return KErrArgument;
		}
	if (!logName.Length())
		{
		logName = pkg.Left(logName.MaxLength() - 4);
		logName.Append(_L(".log"));
		}
	RFs fs;
	TInt r = fs.Connect();
	if (r != KErrNone)
		return r;
	HaveLog = LogFile.Replace(fs, logName, EFileWrite | EFileShareAny) == KErrNone;
	Log(_L("start %S drive %c lang %d"), &pkg, (TUint)drive, lang);

	CHandler* h = new CHandler;
	if (!h)
		r = KErrNoMemory;
	else
		{
		h->iDrive = drive;
		h->iLang = lang;
		// a trap level as TRAP makes it (the cleanup stack refuses to be used without one: E32USER-CBase
		// 66), without its try/catch: this build has no C++ exceptions, so a leave out of here would panic
		// the process (only out of memory can leave; the log shows how far it got). Install returns its
		// result, the handler's functions never leave.
		TTrapHandler* trap = User::MarkCleanupStack();
		CInstallPrefs* prefs = CInstallPrefs::NewL();
		r = Launcher::Install(*h, pkg, *prefs);
		delete prefs;
		User::UnMarkCleanupStack(trap);
		delete h;
		}
	Log(_L("result %d"), r);
	if (HaveLog)
		LogFile.Close();
	fs.Close();
	return r;
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* tc = CTrapCleanup::New();		// for the launcher's own TRAPs and cleanup stack
	CActiveScheduler* as = new CActiveScheduler;	// the launcher's client side runs active objects
	if (!tc || !as)
		return KErrNoMemory;
	CActiveScheduler::Install(as);
	TInt r = Run();
	delete as;
	delete tc;
	return r;
	}

// Startup glue called from startup.s (see src/n95apps/dsbtest/dsbtest.cpp).
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
