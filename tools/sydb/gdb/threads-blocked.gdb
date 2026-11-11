# threads-blocked.gdb: every thread of the kernel's thread container, its process, the kernel object it waits on
# (DThread::iWaitObj: a semaphore, mutex or condition variable, with its name) and the nanokernel wait object
# (NThreadBase::iWaitObj) and nanokernel state (NThreadBase iState +77: 0 ready, 1 suspended, 2 fast semaphore
# = User::WaitForRequest, 3 sleep, 4 blocked on a kernel object = iWaitObj, 5 dead, 6 DFC).
# Set $from and $to (thread index range: the dbg channel returns ~55 lines) before sourcing.
# Use (machine stopped, kernel symbols loaded, docs/sydb.md):
#   sydb dbg interrupt; sydb dbg cmd "source tools/sydb/gdb/threads-blocked.gdb"; sydb dbg continue
# Offsets: DObject::iName +20 (HBuf: length word, max length, text), DThread::iWaitObj +372,
# DThread::iOwningProcess, DThread::iNThread +744, NThreadBase::iWaitObj +40 (ptype /o, syborgfx udeb).
# K::Containers is kernel data: gdb's address (code offset applied) is wrong. Runtime address = kernel data
# base (ROM header kernData, 0xC8000000 on the N8) + (its .bss address in the .sym - 0x400000), here
# 0x40188c -> 0xC800188C (the .sym must match the ROM's kernel: a stale .sym gives wrong addresses). Edit the address for another ROM or kernel build.
set $c = ((DObjectCon**)0xc800188c)[0]
set $i = $from
printf "threads: %d\n", $c->iCount
while $i < $c->iCount && $i < $to
  set $t = (DThread*)$c->iObjects[$i]
  set $n = *(int*)((char*)$t + 20)
  set $p = (unsigned)$t->iOwningProcess
  set $pn = 0
  if ($p >= 0xc8000000 && $p < 0xcc000000)
    set $pn = *(int*)($p + 20)
  end
  set $w = *(DObject**)((char*)$t + 372)
  set $nw = *(int*)((char*)$t + 744 + 40)
  printf "%3d %08x ", $i, $t
  if ((unsigned)$pn >= 0xc8000000 && (unsigned)$pn < 0xcc000000)
    if (*(int*)$pn & 0xfffffff) < 256
      output *((char*)$pn + 8)@(*(int*)$pn & 0xfffffff)
    end
  end
  printf "::"
  if ((unsigned)$n >= 0xc8000000 && (unsigned)$n < 0xcc000000)
    if (*(int*)$n & 0xfffffff) < 256
      output *((char*)$n + 8)@(*(int*)$n & 0xfffffff)
    end
  end
  printf "  nstate %d nwait %08x", *(unsigned char*)((char*)$t + 744 + 77), $nw
  if ((unsigned)$w >= 0xc8000000 && (unsigned)$w < 0xcc000000)
    set $wn = *(int*)((char*)$w + 20)
    printf "  waits on %08x ", $w
    if ((unsigned)$wn >= 0xc8000000 && (unsigned)$wn < 0xcc000000)
      if (*(int*)$wn & 0xfffffff) < 256
        output *((char*)$wn + 8)@(*(int*)$wn & 0xfffffff)
      end
    end
  end
  printf "\n"
  set $i = $i + 1
end
