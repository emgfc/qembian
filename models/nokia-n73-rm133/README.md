# Nokia N73 Music Edition (RM-133)

Firmware 4.0839.42.2.1 (sw.txt 27-09-2008), the last N73 release; language pack V05: English, German,
Russian, Bulgarian, Romanian, Ukrainian. Flash files in `roms/RM-133/4.0839.42.2.1/`.
Extends `nokia-n73-rm132` (same platform, kernel code identical to 3.0717 shifted by 0x1000, same exec
numbering and loader structure sizes [V]); only the firmware files, identity and user window differ.

Level L1 (2026-11-10): first boot in English (city, time, date: Select; tutorial: No), idle at 24 s from
the seed (`build/seeds/nokia-n73-rm133`), menu opens, power menu "Switch off!" ends the session in ~4 s.
Open: as RM-132 (FaxModem.exe KERN-EXEC 3, RES_94 and PN_PHONE_INFO 0x15 unanswered).

Differences in the flash files [V]: ROFS1 is its own `SOS_ROFS` partition in the core .fpsx (3.0717: after
the compressed core in `SOS*CORE`), and the language file is a proper `SOS+ROFX` record (V16: a bare
chunk stream). tools/fw/layout.py reads that from the files; the profile declares no layout.
