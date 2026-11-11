# Personality sym92l91: Symbian 9.2 user sides with the 9.1 loader layout

Used by `models/nokia-e71-rm346` (100.07.76, 200.21.118, 300.21.012) and `models/nokia-n81-8gb-rm179`
(11.0.045, 21.0.010): S60 3.1 builds whose EUSER is 9.2's (`tools/personality/check.py` passes against the
sym92 exec map: 261 SWIs) but whose file server's loader passes the 9.1 `TCodeSegCreateInfo` (ends at
`iCodeRelocTable`, 380 bytes, no demand-paging fields).

- Exec tables, SWI return, export directories, driver ABI: sym92's (`exec.map` is sym92's: `base` below).
- Loader structures: 9.1's sizes (`ExecLoaderCodeSegSize` 380, process tail 36), set at boot for ROM header
  personality 8 (`patches/0024-sym92l91-personality.patch`, `kernel/eka/kernel/sexecpers.cpp`).

How it was found: with sym92 the E71 faulted at 0.37 s with `Fserv fault 11` (EMainStartupNoEStart); a KDLL
trace showed EStart's ProcessCreate fields 7 words early (HeapSizeMin where the N95's CodeRelocTable is). With
sym91 (which has this layout) the 9.1 SWI return through ip broke the 9.2 EUSER at once (efile KERN-EXEC 3).
