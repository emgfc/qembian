# Modern HTTPS on the virtual N95: stock TLS stack, community fixes, our options

Research note, 2026-11-06. Marks: [verified] = read from the N95 firmware in `build/n95/` or a
primary source; [inferred] = best guess from documentation, community reports or general Symbian
knowledge, not checked on this firmware.

## 1. What the stock phone has

### Components (all in the core XIP ROM unless noted) [verified: `build/n95/core.json`, ROFS listings]

| Role | File | Notes |
|---|---|---|
| Secure-socket framework | `Sys/Bin/securesocket.DLL` (2 KB) | `CSecureSocket`; loads the protocol DLL by name [inferred: Symbian securesocket loads `ssl.dll` with `RLibrary::Load`, which is why the community can swap it] |
| TLS/SSL protocol | `Sys/Bin/ssl.dll` (29.8 KB, code at 0x806036a8) | Symbian's own `tls` component. Class names in the binary: `CTlsConnection`, `CClientHello`, `CRsaClientKeyExchMsg30/31` (SSL 3.0 / TLS 1.0 variants), `CDhRsa/CDhDsa/CDhAnon/CDhExplicit...KeyExchMsg`, `CRsaCertificateVerifyMsg`. No ECDH, no TLS 1.1/1.2 classes. [verified: strings] |
| TLS provider (crypto, cert checks) | `Sys/Bin/tlsprovider.dll` (26.6 KB, code at 0x805f72e8) | Cipher-suite table, key material ("client write key", "server finished" labels), session cache, `CTlsClntAuthenticate`, policy file `z:\resource\tlsproviderpolicy.ini` (`ClientAuthDlgEnabled = true`). Certificates are reached through the crypto-token framework (`swtlstokentypeplugin.dll`) and the unified cert store. [verified: strings, ROFS1 file] |
| HTTP framework glue | `Sys/Bin/httpsecuresocket.dll` (1 KB) | the HTTP stack's secure-socket filter (browser, download manager) |
| Crypto | `Sys/Bin/cryptography.dll`, `hash.dll`, `random.dll` (+ `cryptography_hw.dll`, `hash_hw.dll`) | Symbian 9.2 crypto: RSA/DSA/DH, AES/3DES/RC4/DES, MD5/SHA-1/HMAC. SHA-256 is absent in 9.2 [inferred: Symbian 9.2 `hash.dll` exports MD2/MD5/SHA-1 only; SHA-2 arrived in 9.3/9.4, matching Nokia's "SHA2 fix" existing only for FP2+] |
| X.509 | `Sys/Bin/PKIXCert.dll`, `PKIXCertBase.dll`, `asn1.dll`, `reccert.dll` | chain validation; sha256WithRSA signatures cannot be verified without SHA-256 [inferred] |
| Cert stores | `Sys/Bin/CertStore.dll`, `filecertstore.dll`, `swicertstore.dll`, `MIDP2CertStore.dll`, `CertManUi.dll`, `CERTSAVER.exe` | `CertSaver` is the app that imports a `.cer/.der` opened from File manager or the browser |
| Session cache | `tlscacheserver.exe`, `tlscacheclient.dll` | ROFS1 `resource/TlsCacheServer/TlsCacheTimeouts.RSC` |

### Cert store files [verified]

- `z:\private\101F72A6\CACerts.dat` (25 886 B, ROFS2 at 0x728c24) and `z:\private\101F72A6\certclients.dat`
  (240 B). `0x101F72A6` is the cert-store server. On first use the file cert store copies them to
  `c:\private\101f72a6\cacerts.dat` / `certclients.dat` and uses the C: copy from then on [inferred:
  Symbian docs "if it cannot be found the original store is copied over from ROM"].
- Format: Symbian `CPermanentFileStore` (header UIDs `0x10000050 0x00000000 0x045A7094`), streams
  holding DER certs plus trust metadata (label, subject-key hash, list of trusting application UIDs).
- `certclients.dat` names the trusting applications: `Application Controller` (SWI),
  `Internet 0x1000183d`, `Java installer 0x101f9b28`, `OCSP 0x1000a8b6`, `VPN 0x101f7993`. A root is
  trusted for TLS when its entry lists `0x1000183d`.
- Stock roots (15): Baltimore CyberTrust Mobile Root (exp. 2020), Baltimore CyberTrust Root (2025),
  Entrust.net Secure Server x2, Entrust.net 2048, Equifax Secure Global eBusiness (2020), Equifax
  Secure CA (2018), GlobalSign Root CA (2014, truncated copy), GlobalSign R2 (2021), Starfield Class 2
  (2034), Go Daddy Class 2 (2034), Thawte Server CA, Thawte Premium Server CA, GeoTrust Global CA
  (2022), Nokia Root CA (2016). Nothing from ISRG, DigiCert, Google Trust Services, Amazon, Sectigo /
  USERTrust, Comodo. Only the two Class 2 roots and Entrust 2048 are still valid.
- Separate stores not relevant to TLS: `z:\resource\swicertstore.dat` (SIS signing roots, 6 022 B) and
  ROFS1 `private/10203636/security/trustroots/device/certificates/*.cer` (Java/content signing).

### Protocol versions and cipher suites [verified: table at 0x58e0.. in `tlsprovider.dll`, 29 rows of 14 words]

Protocols: SSL 3.0 and TLS 1.0 only (message classes `...30`/`...31`; no 1.1/1.2 code, no
extension handling in `ssl.dll` strings). No SNI [inferred: Symantec's "browsers without SNI" list
names the Nokia S60 browser; consistent with the 1.0-only stack].

Suite table (id, key exchange, cipher, MAC, enabled, priority 1 = preferred):

| enabled, in order of priority | suites |
|---|---|
| 1 `0x0035` RSA AES-256-CBC-SHA, 2 `0x002F` RSA AES-128-CBC-SHA, 3 `0x000A` RSA 3DES-SHA, 4 `0x0016` DHE-RSA 3DES-SHA, 5 `0x0013` DHE-DSS 3DES-SHA, 6 `0x0005` RSA RC4-128-SHA, 7 `0x0004` RSA RC4-128-MD5, 8 `0x0009` RSA DES-SHA, 9 `0x0012` DHE-DSS DES-SHA, 10 `0x0008` RSA EXPORT DES40, 11 `0x0003` RSA EXPORT RC4-40-MD5, 12 `0x0011` DHE-DSS EXPORT, 13 `0x0014` DHE-RSA EXPORT, 14 `0x0019` DH-anon EXPORT | present but disabled: `0x0001/0x0002` NULL, `0x0006/0x0007`, `0x000B-0x0010` DH-DSS/DH-RSA, `0x0015`, `0x0017/0x0018/0x001A/0x001B` |

So: no ECDHE, no SHA-256 MACs, no AES-GCM, no TLS 1.2 PRF. A modern server (TLS 1.2+ only,
sha256WithRSA chain, often ECDSA leaf) fails three times over: version, chain signature algorithm,
missing root.

## 2. What the community does

Sources: GitHub issue "SHA2 (SHA256) issue fix" in mrRosset/Symbian-Archive
(https://github.com/mrRosset/Symbian-Archive/issues/9, 2017-2025, N95 8GB reported working in Mar
2025); nnproject TLS page (https://nnproject.cc/tls/, mirror https://nnp.nnchan.ru/tls/);
All About Symbian links page (https://allaboutsymbian.com/links/) which calls the nnproject patch
"the single most important install".

1. **Nokia's own SHA-2 fix** `networking_improvements.SIS` (dl.nokia.com/ns/symfix/) adds SHA-256 to the
   stock PKIX/crypto. Works on S60v3 FP2 and later only, not FP1 (the N95 8GB). Dead end for us.
   [verified: issue thread]
2. **Root-cert packs.** Replacement `CACerts.dat` bundles (NuruDashdamir 2021/2022 zips in the thread;
   `SHA2Support+Certificates_S60v3-S60v5.zip` in github.com/NuruTasDemir/symbian-mods-new) copied to
   `c:\private\101f72a6\cacerts.dat` on a platsec-hacked phone, or `.cer` files installed one by one via
   File manager (CertSaver). Host-side builder: **mo-krauti/symbian-certapp**
   (https://github.com/mo-krauti/symbian-certapp, release 2024.11.2): Symbian's `certapp` PC tool in a
   Docker image (Ubuntu 6.06), `python create_cacerts_txt.py` turns the host's `/etc/ssl/certs/*.pem`
   into `cacerts.txt`, then `docker run ... symbian-certapp --hca=cacerts.txt --out --bca=cacerts.dat`.
   Certs alone do not give TLS 1.2 and (on FP1) not SHA-256 chains either. [verified: repo READMEs]
3. **Patched TLS stack, the real fix: shinovon's `symbian-tls`** (https://github.com/shinovon/symbian-tls,
   EPL-2.0; mbedTLS port https://github.com/shinovon/mbedtls-symbian). A rewrite of `ssl.dll` on top of
   mbedTLS: TLS 1.2 (TLS 1.3 optional, default since v1.4.4), client mode, system-wide, so the stock
   browser, HTTP framework and J2ME all get it (not Qt). Supported: Symbian 9.1-9.5 including S60v3 FP1
   [verified: README version list; N95 8GB success report in the issue thread]. Distribution: `mbedtls.sis`
   (installs `mbedtls.dll` plus CA roots, updated with every release, latest 2026-10-20) and `ssl.zip`
   "extracted to the C: drive root" (i.e. `c:\sys\bin\ssl.dll`, which the loader takes before `z:`)
   [inferred on the exact path: the page says "extract to C:", the loader searches C: before Z:].
   Certificates are **not** the system store: own bundle in the mbedtls resource dir, user roots in
   `C:\resource\mbedtls\cacerts` (v1.3); the system cert UI shows stub details. Needs on a stock phone:
   InstallServer patch for the SIS, Open4All/ROMPatcher+ to write `c:\sys\bin`. We need none of that:
   we own the C: image and the ROM. Built with Carbide/RVCT 2.2 (ABIv2 `.dso` provided for linking).
4. **Phorepoc/tls** (https://github.com/Phorepoc/tls, Apache-2.0): a second, source-first rewrite of
   `SSL.DLL` ("same exports as the original") plus `libssl.dll` (OpenSSL 0.9.8 API for Open C) over
   mbedTLS 3.6, Mozilla roots pinned by hash, `\resource\ssl\ssl.ini` (`tls13 = 0`). Tested only on a
   Belle phone; S60 3rd "not tested yet". Install on a hacked phone: `SSL.DLL` on C:/E:/F: plus a hash in
   `C:\sys\hash\ssl.dll`. Its build system is Phorepoc's own (`build_mmp_x32.py`, needs their PDK/core
   trees). Good reference source for the Symbian-side glue if we ever build our own.
5. **Proxies.** `oldssl-proxy` by ValdikSS (https://bitbucket.org/ValdikSS/oldssl-proxy): a Docker
   TLS-terminating HTTP proxy that issues its own CA (install it on the device), speaks old TLS to the
   phone and modern TLS outwards; tested with an E5-00 on a LAN. Opera Mini (J2ME) uses Opera's own
   OBML proxy with its own trust list, independent of the OS store (Let's Encrypt forum note); its old
   server generations are largely gone [inferred]. Neither gives the system browser TLS 1.2.
6. Other: JigokuMaster/Symbian-TLS-Patch (UIQ3 flavour of the same patch), janseris/symbian-tls (fork),
   Open C's `libssl` (OpenSSL 0.9.8, TLS 1.0 only, Open C apps only), curl notes from 2008 that libcurl
   never used Symbian's SSL.

## 3. Our options

Our networking stand-in today: **none on the user side**. The QEMU board has a `virtio-net` device at
`0xc000c000` (IRQ 9, `docs/architecture.md`) and nothing drives it. The N95 ROM has no Ethernet
PDD/NIF (no `ethernet.pdd`, `ethint.nif`); its IP paths are packet data over the modem
(`GPRSUMTSAgent.agt`, `psd.agt`, `spud.nif`, `rawip.nif`, `ppp.nif`, `DATAPORT.CSY`, through the ISC/ISI
link our fake modem answers) and WLAN over Nokia's real-hardware `wlanldd.ldd`/`wlanpdd.pdd`
(`wlanhwinit.dll`, "not analysed" in `docs/n95-isc-isi-protocol.md`). `docs/roadmap.md` and
`docs/hacks.md` have no network entry. So any HTTPS plan has a prerequisite: an IP path. Cheapest
candidate [inferred]: teach the fake modem packet data (GPDS context activation + PN_PIPE + DATAPORT
raw-IP frames) and bridge the frames to a QEMU `-netdev user` (slirp) backend, which NATs through the
host with no proxy at all. The alternative, an Ethernet PDD for virtio-net plus Symbian's
`ether802`/`ethint` NIF, needs the EPL Symbian networking sources (licence note for the owner) and a
CommsDat IAP. Either way is its own research brief.

### (a) Replace the root certs only

- How: build `cacerts.dat` on the host with mo-krauti/symbian-certapp (Docker, Symbian `certapp`),
  or add `.cer` files on the phone via `CERTSAVER.exe` (open the file; `sydb fs_push` + `app_run`) and
  then pull `c:\private\101f72a6\cacerts.dat`. Ship it on `drive-c.img` (mtools, as `sydb install` does)
  or replace the ROFS2 file.
- Cost: hours. Coverage: nothing on its own. The stock stack still speaks TLS 1.0 without SNI and
  cannot verify SHA-256 chain signatures [inferred], so no public site works. Useful only together with (c).

### (b) Replace the TLS stack

- How, cheapest: take shinovon's EKA2 release (`ssl.zip` + `mbedtls.sis` from nnproject.cc/tls), unpack
  on the host, put `ssl.dll`, `mbedtls.dll` and the resource files on `drive-c.img` under `c:\sys\bin`
  and `c:\resource\...` (no InstallServer or platsec hack needed; we write the image directly or via
  `sydb fs_push`). The loader prefers `c:\sys\bin\ssl.dll` over the XIP `z:` copy [inferred; this is what
  the community relies on]. Check the DLL header with `tools/e32load.py`: capabilities must cover every
  client process (browser: NetworkServices, ReadUserData...; the patch is built with ALL -TCB, as all
  system DLLs are [inferred]). Alternative if the loader does take Z: first: `tools/romcompose.py
  --file sys/bin/ssl.dll=...` can only place files in the new root dir; a non-XIP E32Image needs
  conversion to XIP (rombuild-style relocation) that romcompose does not do today, or a ROFS2 swap.
- Cost: a day, mostly verifying 9.2 compatibility (reports exist for N95 8GB) and that `mbedtls.dll`
  resolves (v1.4.0+ no longer needs PIPS/Open C). Source alternative if the binaries misbehave:
  Phorepoc/tls (Apache-2.0, needs their toolchain) or shinovon's source (EPL-2.0, Carbide/RVCT
  project; a GCC-symbianelf build is unproven).
- Coverage: TLS 1.2/1.3, modern suites and current roots for the stock browser, HTTP framework, J2ME.
  Not Qt (none on the N95 anyway). Cert UI shows stub details.

### (c) Host-side TLS-terminating proxy

- How: QEMU slirp (or our frame bridge) forwards all TCP to a host proxy (oldssl-proxy, or a small
  mitmproxy/own script) that terminates with our CA, offers TLS 1.0 + `0x0035`/`0x002F` and SHA-1
  signed leaf certs to the phone, and speaks modern TLS outwards. Our CA root goes into the store via
  (a). The browser needs the proxy in its IAP (CommsDat `Proxies` table) or we intercept port 443
  transparently and the phone still thinks it talks TLS 1.0 to the origin.
- Cost: a day once the IP path exists; plus (a). Coverage: everything, including apps with their own
  pinned expectations, but every connection is downgraded to SHA-1/TLS 1.0 on the phone side and the
  host must re-sign each site (fine on QEMU, no security concern).
- Value: the only route if (b) cannot be made to load on 9.2; also a good debugging aid (the proxy
  logs every request).

### (d) Port rustls [inferred throughout; added on the owner's mid-brief question]

- There is no Rust target for Symbian. A port would be: nightly `-Z build-std` with a custom target
  JSON (ARMv6, soft-float, EABI, `panic=abort`, no OS), `rustls` in `no_std` + `alloc` mode (supported
  since 0.23) with a pure-Rust crypto provider (`rustls-rustcrypto`; `ring`/`aws-lc-rs` bring C and asm
  and `rustls-ffi` needs `std`, so both are out), a `GlobalAlloc` over `User::Alloc`, a hand-written C
  ABI, time and RNG hooks, and then exactly the same C++ Symbian glue that shinovon/Phorepoc wrote for
  mbedTLS (`CSecureSocket` provider over `RSocket` with active objects, the record pump, hostname/SNI
  option, cert dialogs). Rust's ELF objects (AAPCS, soft-float) should link with our GCC symbianelf.
- Cost: weeks, and the output is functionally what option (b) already delivers (mbedTLS 3.6 gives
  TLS 1.3, ECDHE, AES-GCM, ChaCha20). Memory safety buys nothing on a single-user emulator. Code size
  of a build-std rustls is ~1 MB, acceptable. Not stupid, just not cheaper than anything above; keep it
  as a fallback only if both mbedTLS-based DLLs turn out unusable on 9.2 and their sources won't build.

## 4. Recommendation and next steps

Go with **(b) shinovon's ssl.dll + mbedtls.dll on the C: drive** (zero build, proven on an N95 8GB),
and fold (a) in only as far as that patch's own CA bundle. Keep (c) as the fallback and as a traffic
debugger. Do not start (d).

1. Network first (blocking everything): research brief for packet data in the fake modem (GPDS +
   PN_PIPE + DATAPORT raw IP) bridged to QEMU slirp; `docs/n95-isc-isi-protocol.md` has the ISI
   infrastructure. Deliverable: the browser resolves a name and fetches a plain `http://` page.
2. Fetch nnproject `ssl.zip` and `mbedtls.sis`; unpack (SIS: `tools/` sis extractor or sydb install),
   inspect headers with `tools/e32load.py` (UIDs, caps, imports: must be only euser/esock/efsrv/mbedtls),
   diff the export count against the stock `ssl.dll` (416 symbols in our ELF dump, ordinal 1 is the
   factory the framework calls).
3. Put the files on `drive-c.img` (`c:\sys\bin\ssl.dll`, `c:\sys\bin\mbedtls.dll`, the resource dir);
   boot; `sydb log --grep ssl.dll` / `sydb strace` to confirm the loader took the C: copy. If it
   insists on Z:, swap the ROM copy instead (ROFS2 rebuild or a romcompose XIP conversion).
4. Open `https://www.cdn77.com/tls-test` and a Let's Encrypt site in the stock browser; check with
   `sydb catch` for crashes in the browser process.
5. If step 3/4 fails on 9.2: oldssl-proxy on the host + our CA in `cacerts.dat` built with
   mo-krauti/symbian-certapp, proxy set in the IAP.

Open questions: exact resource paths inside `ssl.zip`/`mbedtls.sis` (not read yet); whether the EKA2
loader really prefers C: for a DLL that also exists in XIP ROM on 9.2 (community practice says yes);
capability bits of the patched DLL.

## Sources

- Firmware: `build/n95/core.json`, `build/n95/rofs1.img`, `rofs2.img` (`tools/rofs.py`), DLLs extracted
  with `tools/xip2elf.py`; cert store files dumped with `openssl x509`.
- https://github.com/mrRosset/Symbian-Archive/issues/9
- https://nnproject.cc/tls/ and https://github.com/shinovon/symbian-tls, https://github.com/shinovon/mbedtls-symbian
- https://github.com/Phorepoc/tls
- https://github.com/mo-krauti/symbian-certapp
- https://bitbucket.org/ValdikSS/oldssl-proxy
- https://github.com/JigokuMaster/Symbian-TLS-Patch
- https://allaboutsymbian.com/links/
- Symbian SDK docs on cert stores (akawolf.org mirror, "Root Certificates", "Security Tools Overview": certapp, cacerts.dat/certclients.dat)
- https://community.letsencrypt.org/t/cert-error-in-opera-mini-browser/20737 (Opera Mini trust list)
- https://support.symantec.com/us/en/article.TECH235487.html (S60 browser lacks SNI)
