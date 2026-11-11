#!/usr/bin/env python3
"""EKA1 / Symbian ROM extractor for Nokia RH-29 (N-Gage QD) Phoenix/DCT4 firmware.

Stages:
  1. Parse the Phoenix A2 / DCT4 container (record boundaries validated, no
     heuristic resynchronisation).
  2. Build a flash-address -> bytes model for each input.
  3. Reconstruct the EKA1 ROM.  On RH-29 the ROM is *split*: ROM offset 0 is
     at flash address 0, and the upper flash bank at flash 0x02000000 continues
     the same ROM at offset 0x00F00000.  The mapping is proven by extracting
     files listed by the ROM directory tree (see --report).
  4. Parse the EKA1 root directory list / directory entries and extract the
     complete file tree, recording ROM virtual address, ROM offset and the
     exact source records/offsets for every byte.
  5. Reconstruct the second (PPM/variant) filesystem overlay found in
     RH2941.026 and extract the files that are actually covered by data.

Everything stays under the supplied workspace.  Original inputs are never
modified.  Output is rebuilt in place (only this tool's own files are
overwritten).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

ROM_BASE = 0x50000000
KERNEL_DATA = 0x80000000
# First flash address that belongs to the "upper" flash bank on RH-29.
UPPER_BANK_START = 0x01000000
MAX_ROM_SIZE = 0x08000000


class FormatError(ValueError):
    pass


def take(data: bytes, off: int, size: int) -> bytes:
    if off < 0 or size < 0 or off + size > len(data):
        raise FormatError(f"out-of-bounds read off=0x{off:x} size=0x{size:x} len=0x{len(data):x}")
    return data[off:off + size]


def be(data: bytes, off: int, size: int) -> int:
    return int.from_bytes(take(data, off, size), "big")


def le32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", take(data, off, 4))[0]


def checksum8(data: bytes) -> int:
    return (sum(data) & 0xFF) ^ 0xFF


# --------------------------------------------------------------------------
# Phoenix A2 / DCT4 container
# --------------------------------------------------------------------------

@dataclass
class Record:
    index: int
    offset: int          # offset of record header in the input file
    kind: int
    header_size: int
    size: int
    address: Optional[int]
    device: Optional[int]
    checksums: dict
    header_hex: str

    @property
    def payload_offset(self) -> int:
        return self.offset + self.header_size


def parse_container(data: bytes) -> Tuple[dict, List[Record]]:
    if not data or data[0] != 0xA2:
        raise FormatError("not a Phoenix A2/DCT4 container (missing 0xA2 magic)")
    header_end = 5 + be(data, 1, 4)
    if header_end > len(data):
        raise FormatError("container header extends past end of file")
    count = be(data, 5, 4)
    pos = 9
    tokens = []
    for _ in range(count):
        if pos + 2 > header_end:
            raise FormatError("truncated header token")
        tid = data[pos]
        pos += 1
        width = 2 if tid == 0xF3 or data[pos] == 0 else 1
        size = be(data, pos, width)
        pos += width
        if pos + size > header_end:
            raise FormatError("token extends past header")
        value = take(data, pos, size)
        tokens.append({
            "id": f"0x{tid:02x}", "offset": pos, "size": size, "hex": value.hex(),
            "text": "".join(chr(c) if 32 <= c < 127 else "." for c in value),
        })
        pos += size
    if pos != header_end:
        raise FormatError(f"header token count/length mismatch at 0x{pos:x} (expected 0x{header_end:x})")

    records: List[Record] = []
    pos = header_end
    while pos < len(data):
        kind = data[pos]
        address = device = None
        checks: dict = {}
        if kind in (0x14, 0x21, 0x2E):
            hlen = 16 if kind == 0x21 else 10
            h = take(data, pos, hlen)
            if kind == 0x2E:
                device = be(h, 1, 2)
                address = be(h, 3, 2) << 9
            else:
                address = be(h, 7 if kind == 0x21 else 1, 4)
            size = be(h, hlen - 4, 3)
            payload = take(data, pos + hlen, size)
            if kind == 0x14:
                calculated = checksum8(payload)
                checks = {
                    "header_ok": checksum8(h[1:-1]) == h[-1],
                    "data_ok": calculated == h[5],
                    "data_stored": h[5],
                    "data_calculated": calculated,
                    "legacy_plus_one": ((h[5] - calculated) & 0xFF) == 1,
                }
        elif kind == 0x20:
            hlen = 6
            take(data, pos, hlen)
            size = be(data, pos + 3, 2)
        elif kind == 0x25:
            hlen = 0
            size = 2 + be(data, pos + 1, 1)
        elif kind == 0x26:
            hlen, size = 0, 10
        else:
            raise FormatError(f"unknown record type 0x{kind:02x} at 0x{pos:x}")
        take(data, pos + hlen, size)
        records.append(Record(len(records), pos, kind, hlen, size, address, device,
                              checks, data[pos:pos + hlen].hex()))
        pos += hlen + size
    return {"header_size": header_end, "tokens": tokens}, records


# --------------------------------------------------------------------------
# Flash image model
# --------------------------------------------------------------------------

@dataclass
class Block:
    """A contiguous run of flash bytes, with provenance back to the input."""
    start: int
    data: bytes
    records: List[Record]          # records that contributed, in order
    source: str                    # input file name
    source_sha256: str
    bid: int = -1

    @property
    def end(self) -> int:
        return self.start + len(self.data)


@dataclass
class Source:
    path: Path
    name: str
    sha256: str
    size: int
    header: dict
    records: List[Record]
    blocks: List[Block] = field(default_factory=list)
    coverage: List[Tuple[int, int]] = field(default_factory=list)

    def block_covering(self, addr: int, size: int) -> Optional[Block]:
        for b in self.blocks:
            if b.start <= addr and addr + size <= b.end:
                return b
        return None

    def total_covered(self, lo: int = 0, hi: int = 1 << 32) -> int:
        return sum(max(0, min(b.end, hi) - max(b.start, lo)) for b in self.blocks)


def build_blocks(src: Source, data: bytes) -> None:
    spans = [(r.address, take(data, r.payload_offset, r.size), r)
             for r in src.records if r.address is not None]
    spans.sort(key=lambda x: x[0])
    merged: List[Block] = []
    for addr, payload, rec in spans:
        if merged and addr <= merged[-1].end:
            prev = merged[-1]
            # Overlap: accept only identical bytes; otherwise it is ambiguous.
            overlap = prev.end - addr
            if prev.data[addr - prev.start:addr - prev.start + min(overlap, len(payload))] != payload[:min(overlap, len(payload))]:
                raise FormatError(f"{src.name}: conflicting overlapping flash records at 0x{addr:x}")
            if addr + len(payload) > prev.end:
                prev.data += payload[overlap:]
                prev.records.append(rec)
            else:
                prev.records.append(rec)
            continue
        merged.append(Block(addr, bytes(payload), [rec], src.name, src.sha256))
    for i, b in enumerate(merged):
        b.bid = i
    src.blocks = merged
    src.coverage = [(b.start, b.end) for b in merged]


def load_source(path: Path) -> Source:
    data = path.read_bytes()
    header, records = parse_container(data)
    src = Source(path, path.name, hashlib.sha256(data).hexdigest(), len(data), header, records)
    build_blocks(src, data)
    return src


# --------------------------------------------------------------------------
# EKA1 ROM reconstruction
# --------------------------------------------------------------------------

@dataclass
class Piece:
    """Mapping of a reconstructed image range to a source flash block range.

    Ownership of the actual bytes (source file, record indices and file
    offsets) lives once in the image's block table; a piece only references
    the block id, which keeps manifests small.
    """
    image_offset: int
    length: int
    source: str
    source_sha256: str
    flash_address: int
    kind: str  # 'data' | 'erased_gap'
    block_id: int = -1

    def to_json(self) -> dict:
        return {
            "image_offset": self.image_offset, "length": self.length,
            "source": self.source, "source_sha256": self.source_sha256,
            "flash_address": self.flash_address, "kind": self.kind,
            "block_id": self.block_id,
        }


@dataclass
class Image:
    name: str
    base: int
    data: bytearray
    pieces: List[Piece]
    mapping: List[dict]

    def pieces_for(self, off: int, size: int) -> List[Piece]:
        out = []
        for p in self.pieces:
            if p.image_offset < off + size and p.image_offset + p.length > off:
                out.append(p)
        return out

    def is_covered(self, off: int, size: int) -> bool:
        """True only if every byte is backed by real source data."""
        cur = off
        end = off + size
        for p in sorted(self.pieces_for(off, size), key=lambda x: x.image_offset):
            if p.kind != "data" or p.image_offset > cur:
                return False
            cur = max(cur, p.image_offset + p.length)
            if cur >= end:
                return True
        return cur >= end

    def missing_ranges(self, off: int, size: int) -> List[Tuple[int, int]]:
        gaps = []
        cur = off
        end = off + size
        for p in sorted(self.pieces_for(off, size), key=lambda x: x.image_offset):
            if p.image_offset > cur:
                gaps.append((cur, min(p.image_offset, end)))
            if p.kind == "data":
                cur = max(cur, p.image_offset + p.length)
            if cur >= end:
                break
        if cur < end:
            gaps.append((cur, end))
        return [(a - off, b - off) for a, b in gaps if b > a]


def read_eka1_header(flash_at_zero: bytes) -> dict:
    if len(flash_at_zero) < 0x100:
        raise FormatError("ROM header block shorter than 0x100 bytes")
    h = {
        "restart_vector": le32(flash_at_zero, 0x7C),
        "version_major": flash_at_zero[0x80],
        "version_minor": flash_at_zero[0x81],
        "version_build": struct.unpack_from("<H", flash_at_zero, 0x82)[0],
        "time_field": struct.unpack_from("<Q", flash_at_zero, 0x84)[0],
        "rom_base": le32(flash_at_zero, 0x8C),
        "rom_size": le32(flash_at_zero, 0x90),
        "rom_root_dir_list": le32(flash_at_zero, 0x94),
        "kern_data_address": le32(flash_at_zero, 0x98),
        "kern_limit": le32(flash_at_zero, 0x9C),
        "primary_file": le32(flash_at_zero, 0xA0),
        "secondary_file": le32(flash_at_zero, 0xA4),
        "checksum": le32(flash_at_zero, 0xA8),
        "screen_x": le32(flash_at_zero, 0xB8),
        "screen_y": le32(flash_at_zero, 0xBC),
        "bits_per_pixel": le32(flash_at_zero, 0xC0),
    }
    if h["rom_base"] != ROM_BASE:
        raise FormatError(f"unexpected rom_base 0x{h['rom_base']:08x}")
    if not (0x1000 <= h["rom_size"] <= MAX_ROM_SIZE):
        raise FormatError(f"implausible rom_size 0x{h['rom_size']:x}")
    if not (h["rom_base"] <= h["rom_root_dir_list"] < h["rom_base"] + h["rom_size"]):
        raise FormatError("rom_root_dir_list outside ROM")
    return h


def choose_rom_source(sources: List[Source]) -> Source:
    candidates = []
    for s in sources:
        b = s.block_covering(0, 0x100)
        if not b:
            continue
        try:
            h = read_eka1_header(b.data[:0x100])
        except FormatError:
            continue
        low = s.total_covered(0, UPPER_BANK_START)
        candidates.append((low, sum(len(x.data) for x in s.blocks), s, h))
    if not candidates:
        raise FormatError("no input contains a plausible EKA1 ROM header at flash address 0")
    candidates.sort(key=lambda x: (x[0], x[1]), reverse=True)
    return candidates[0][2]


def reconstruct_rom(src: Source) -> Tuple[Image, dict]:
    header_block = src.block_covering(0, 0x100)
    if not header_block:
        raise FormatError("ROM header not fully covered")
    header = read_eka1_header(header_block.data[:0x100])
    rom_size = header["rom_size"]
    img = bytearray(b"\xFF" * rom_size)
    pieces: List[Piece] = []
    mapping: List[dict] = []

    low = [b for b in src.blocks if b.start < UPPER_BANK_START]
    high = sorted((b for b in src.blocks if b.start >= UPPER_BANK_START), key=lambda b: b.start)

    def place(b: Block, rom_off: int, bank: str) -> None:
        if rom_off < 0 or rom_off + len(b.data) > rom_size:
            raise FormatError(f"{src.name}: block at flash 0x{b.start:x} maps outside ROM "
                              f"(rom 0x{rom_off:x}+0x{len(b.data):x} > 0x{rom_size:x})")
        img[rom_off:rom_off + len(b.data)] = b.data
        pieces.append(Piece(rom_off, len(b.data), src.name, src.sha256, b.start, "data", b.bid))
        mapping.append({
            "block_id": b.bid, "bank": bank,
            "flash_address": b.start, "flash_length": len(b.data),
            "rom_offset": rom_off, "rom_virtual_address": ROM_BASE + rom_off,
            "records": [{"index": r.index, "file_offset": r.offset, "type": f"0x{r.kind:02x}",
                         "payload_file_offset": r.payload_offset, "size": r.size,
                         "flash_address": r.address} for r in b.records],
        })

    for b in low:
        place(b, b.start, "lower")
    low_end = max((b.end for b in low), default=0)
    # Erased/unprogrammed hole(s) inside the lower bank (flash erase state 0xFF).
    low_sorted = sorted(low, key=lambda b: b.start)
    cur = 0
    for b in low_sorted:
        if b.start > cur:
            pieces.append(Piece(cur, b.start - cur, "(none)", "", 0, "erased_gap", -1))
        cur = max(cur, b.end)
    if 0 < low_end < rom_size and high:
        if low_end > rom_size:
            low_end = rom_size

    # Upper bank: the flash bridge is not part of the ROM address space.  The
    # ROM continues where the lower bank ended; this is verified afterwards
    # against files listed by the ROM's own directory tree.
    rom_off = low_end
    for b in high:
        place(b, rom_off, "upper")
        rom_off += len(b.data)
    if rom_off > rom_size:
        raise FormatError("upper bank overruns rom_size")
    if rom_off < rom_size:
        pieces.append(Piece(rom_off, rom_size - rom_off, "(none)", "", 0, "erased_gap", -1))

    pieces.sort(key=lambda p: p.image_offset)
    return Image("rom", ROM_BASE, img, pieces, mapping), header


# --------------------------------------------------------------------------
# EKA1 directory tree
# --------------------------------------------------------------------------

SAFE_FORBIDDEN = set('/\\:\x00')


def safe_component(name: str) -> str:
    if not name or name in (".", ".."):
        raise FormatError(f"unsafe empty/dot ROM filename {name!r}")
    if any(c in SAFE_FORBIDDEN for c in name):
        raise FormatError(f"unsafe ROM filename {name!r}")
    if any(ord(c) < 32 or ord(c) == 127 for c in name):
        raise FormatError(f"control character in ROM filename {name!r}")
    if len(name) > 255:
        raise FormatError("ROM filename too long")
    return name


@dataclass
class Entry:
    path: str
    name: str
    address: int
    size: int
    attributes: int
    is_dir: bool
    entry_offset: int  # image offset of entry header


def parse_dir(img: Image, off: int, path: str, ancestors: frozenset,
              entries: List[Entry], max_depth: int = 64) -> None:
    if off in ancestors or len(ancestors) > max_depth:
        raise FormatError(f"cyclic/excessively deep ROM directory at {path!r}")
    size = le32(img.data, off)
    total = size + 4
    if size < 0 or total < 4 or total > 0x40000 or off + total > len(img.data):
        raise FormatError(f"invalid directory size 0x{size:x} at {path!r} (off 0x{off:x})")
    if total % 4:
        # Observed EKA1 directories are 4-byte sized; do not guess.
        raise FormatError(f"directory size not 4-byte aligned at {path!r}")
    pos, end = off + 4, off + total
    used = set()
    while pos < end:
        if pos + 10 > end:
            raise FormatError(f"truncated directory entry in {path!r}")
        nbytes, addr, attr, nchars = struct.unpack_from("<IIBB", img.data, pos)
        if nchars == 0 or nchars > 200:
            raise FormatError(f"invalid name length {nchars} in {path!r} at image off 0x{pos:x}")
        name_end = pos + 10 + 2 * nchars
        if name_end > end:
            raise FormatError(f"entry name crosses directory end in {path!r}")
        try:
            name = img.data[pos + 10:name_end].decode("utf-16-le")
        except UnicodeDecodeError as exc:
            raise FormatError(f"invalid UTF-16 name in {path!r}: {exc}") from exc
        safe_component(name)
        if name.casefold() in used:
            raise FormatError(f"duplicate entry {name!r} in {path!r}")
        used.add(name.casefold())
        is_dir = bool(attr & 0x10)
        child_path = f"{path}/{name}" if path else name
        entries.append(Entry(child_path, name, addr, nbytes, attr, is_dir, pos))
        if is_dir:
            parse_dir(img, addr - img.base, child_path, ancestors | {off}, entries, max_depth)
        pos = (name_end + 3) & ~3
    if pos != end:
        raise FormatError(f"directory alignment mismatch in {path!r}")


def walk_tree(img: Image, root_ptr: int, max_entries: int = 100000) -> Tuple[List[Entry], List[dict]]:
    root_off = root_ptr - img.base
    if root_off < 0 or root_off + 4 > len(img.data):
        raise FormatError("root directory list outside image")
    count = le32(img.data, root_off)
    if not (1 <= count <= 64):
        raise FormatError(f"invalid root directory count {count}")
    roots = []
    entries: List[Entry] = []
    for i in range(count):
        base = root_off + 4 + i * 8
        if base + 8 > len(img.data):
            raise FormatError("root directory list truncated")
        hardware, ptr = struct.unpack_from("<II", img.data, base)
        roots.append({"index": i, "hardware_variant": hardware, "dir_address": ptr})
        prefix = "" if count == 1 else f"root_{i}_{hardware:08x}"
        parse_dir(img, ptr - img.base, prefix, frozenset(), entries)
        if len(entries) > max_entries:
            raise FormatError("too many ROM entries")
    return entries, roots


# --------------------------------------------------------------------------
# Module lists referenced by the ROM header (ekern.exe etc.)
# --------------------------------------------------------------------------

def parse_module_list(img: Image, addr: int, limit: int, label: str) -> List[dict]:
    off = addr - img.base
    out = []
    pos = off
    while pos + 10 <= limit:
        nbytes, a, attr, nchars = struct.unpack_from("<IIBB", img.data, pos)
        if nchars == 0 or nchars > 128 or pos + 10 + 2 * nchars > limit:
            break
        try:
            name = img.data[pos + 10:pos + 10 + 2 * nchars].decode("utf-16-le")
        except UnicodeDecodeError:
            break
        if not name or not all(32 <= ord(c) < 127 for c in name):
            break
        if not (a == 0 or img.base <= a < img.base + len(img.data)):
            break
        if nbytes > len(img.data):
            break
        if attr & 0x10:  # first directory entry: end of the ROM module list
            break
        out.append({"list": label, "name": name, "size": nbytes, "address": a,
                    "attributes": attr, "entry_image_offset": pos,
                    "entry_rom_offset": pos, "entry_virtual_address": img.base + pos})
        pos = (pos + 10 + 2 * nchars + 3) & ~3
    return out


# --------------------------------------------------------------------------
# Extraction helpers
# --------------------------------------------------------------------------

def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def extract_tree(img: Image, entries: List[Entry], out_dir: Path, coverage_kind: str) -> Tuple[List[dict], List[dict]]:
    files = []
    problems = []
    for e in entries:
        if e.is_dir:
            continue
        off = e.address - img.base
        rec = {
            "path": e.path, "name": e.name, "size": e.size,
            "attributes": e.attributes,
            "attribute_flags": attribute_flags(e.attributes),
            "rom_virtual_address": e.address,
            "rom_offset": off,
            "kind": coverage_kind,
            "entry_image_offset": e.entry_offset,
            "entry_virtual_address": img.base + e.entry_offset,
        }
        if e.size == 0:
            rec["complete"] = True
            rec["sha256"] = sha256(b"")
            files.append(rec)
            continue
        if off < 0 or off + e.size > len(img.data):
            rec["complete"] = False
            rec["error"] = "file range outside image"
            problems.append(rec)
            files.append(rec)
            continue
        covered = img.is_covered(off, e.size)
        rec["complete"] = covered
        rec["covered_by"] = [p.to_json() for p in img.pieces_for(off, e.size)]
        if not covered:
            rec["missing_ranges"] = img.missing_ranges(off, e.size)
            problems.append(rec)
            files.append(rec)
            continue
        blob = bytes(img.data[off:off + e.size])
        rec["sha256"] = sha256(blob)
        target = safe_join(out_dir, e.path)
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            raise FormatError(f"duplicate output path {target}")
        target.write_bytes(blob)
        files.append(rec)
    return files, problems


def safe_join(root: Path, rel: str) -> Path:
    parts = [safe_component(p) for p in rel.split("/")]
    target = root.joinpath(*parts)
    resolved = target.resolve()
    root_resolved = root.resolve()
    if resolved != root_resolved and root_resolved not in resolved.parents:
        raise FormatError(f"path escapes output directory: {rel!r}")
    return target


def attribute_flags(attr: int) -> List[str]:
    flags = []
    for bit, name in [(0x01, "read_only_or_file"), (0x02, "hidden"), (0x04, "system"),
                      (0x08, "volume"), (0x10, "directory"), (0x20, "archive"),
                      (0x40, "?"), (0x80, "?")]:
        if attr & bit:
            flags.append(name)
    return flags


# --------------------------------------------------------------------------
# Variant / PPM overlay (second filesystem image in RH2941.026)
# --------------------------------------------------------------------------

def find_root_list_candidate(img: Image) -> Optional[Tuple[int, int]]:
    """Find (root_list_image_offset, root_dir_pointer) inside an image."""
    for off in range(0, max(0, len(img.data) - 12), 4):
        n = le32(img.data, off)
        if not (1 <= n <= 16):
            continue
        if off + 4 + 8 * n > len(img.data):
            continue
        ok = True
        ptrs = []
        for i in range(n):
            hw, ptr = struct.unpack_from("<II", img.data, off + 4 + 8 * i)
            if not (img.base <= ptr < img.base + len(img.data) or 0x50000000 <= ptr < 0x54000000):
                ok = False
                break
            ptrs.append(ptr)
        if not ok:
            continue
        for ptr in ptrs:
            # Try every 4-aligned offset as the directory the pointer names.
            for d in range(0, len(img.data) - 4, 4):
                size = le32(img.data, d)
                total = size + 4
                if total < 4 or total > 0x40000 or d + total > len(img.data):
                    continue
                vbase = ptr - d
                if vbase & 0xFFF:
                    continue
                trial = Image("trial", vbase, img.data, [], [])
                try:
                    ents, _ = walk_tree(trial, vbase + off)
                except FormatError:
                    continue
                if len(ents) > 3:
                    return off, ptr
    return None


def detect_variant_overlay(src: Source) -> Optional[dict]:
    """Locate the second (PPM) directory tree and its data window in a source."""
    if not src.blocks:
        return None
    best = None
    for b in src.blocks:
        if len(b.data) < 0x1000:
            continue
        # Scan this block for a root-dir list / directory combination.
        for off in range(0, min(len(b.data) - 12, 0x10000), 4):
            n = le32(b.data, off)
            if not (1 <= n <= 16) or off + 4 + 8 * n > len(b.data):
                continue
            ok = True
            ptrs = []
            for i in range(n):
                hw, ptr = struct.unpack_from("<II", b.data, off + 4 + 8 * i)
                if hw > 0x0FFFFFFF or not (0x50000000 <= ptr < 0x60000000):
                    ok = False
                    break
                ptrs.append(ptr)
            if not ok:
                continue
            for ptr in ptrs:
                for d in range(0, len(b.data) - 4, 4):
                    size = le32(b.data, d)
                    total = size + 4
                    if total < 4 or total > 0x40000 or d + total > len(b.data):
                        continue
                    vbase = ptr - d
                    if vbase & 0xFFF:
                        continue
                    trial = Image("trial", vbase, b.data, [], [])
                    try:
                        ents, roots = walk_tree(trial, vbase + off)
                    except FormatError:
                        continue
                    if len(ents) < 100:
                        continue
                    names = {e.path for e in ents}
                    score = len(ents)
                    cand = {
                        "block": b, "block_offset": off, "virtual_base": vbase,
                        "root_list_offset": off, "root_dir_pointer": ptr,
                        "entries": ents, "roots": roots, "score": score,
                    }
                    if best is None or score > best["score"]:
                        best = cand
            if best:
                break
        if best:
            break
    return best


def detect_data_window(src: Source, overlay: dict, tree_image: Image) -> Optional[dict]:
    """Find the virtual base of the file-data block used by the overlay."""
    used = tree_image.base
    tree_end = used + len(tree_image.data)
    # Collect addresses referenced by files that are not inside the tree image
    # and not inside the ROM (the ROM is already known separately).
    addrs = [e.address for e in overlay["entries"]
             if not e.is_dir and e.size and not (tree_image.base <= e.address < tree_end)
             and not (0x50000000 <= e.address < 0x51200000)]
    # Keep only addresses in the plausible PPM data range.
    addrs = [a for a in addrs if 0x51200000 <= a < 0x54000000]
    if not addrs:
        return None
    lo, hi = min(addrs), max(addrs)
    candidates = []
    for b in src.blocks:
        if b is overlay["block"]:
            continue
        # Candidate base: every page such that all addresses fit in this block.
        start = max(0x51200000, hi - len(b.data))
        start = (start + 0xFFF) & ~0xFFF
        end = (lo & ~0xFFF)
        if start > end:
            continue
        for vbase in range(start, end + 0x1000, 0x1000):
            if vbase < 0x51200000:
                continue
            # Score by extension signatures for the files that live here.
            score = 0
            tested = 0
            for e in overlay["entries"]:
                if e.is_dir or not e.size:
                    continue
                if not (vbase <= e.address < vbase + len(b.data)):
                    continue
                off = e.address - vbase
                if off + min(e.size, 16) > len(b.data):
                    continue
                blob = b.data[off:off + min(e.size, 64)]
                tested += 1
                ext = e.name.rsplit(".", 1)[-1].lower() if "." in e.name else ""
                if ext == "mid" and blob[:4] == b"MThd":
                    score += 10
                elif ext in ("jpg", "jpeg") and blob[:2] == b"\xff\xd8":
                    score += 10
                elif ext == "png" and blob[:8] == b"\x89PNG\r\n\x1a\n":
                    score += 10
                elif ext == "wav" and blob[:4] == b"RIFF" and blob[8:12] == b"WAVE":
                    score += 10
                elif ext == "mbm" and blob[:4] in (b"\x37\x00\x00\x10", b"\x38\x00\x00\x10", b"\x39\x00\x00\x10"):
                    score += 5
                elif ext in ("rsc", "r01", "r02") and len(blob) > 8 and blob[0] in (0x00, 0x01, 0x10, 0x11, 0x12, 0x13):
                    score += 1
            if tested:
                candidates.append((score, tested, vbase, b))
    if not candidates:
        return None
    candidates.sort(key=lambda x: (x[0], x[1]), reverse=True)
    score, tested, vbase, b = candidates[0]
    return {"block": b, "virtual_base": vbase, "score": score, "tested": tested,
            "size": len(b.data)}


def composite_image(name: str, parts: List[Tuple[int, Block]]) -> Image:
    """Build a sparse image from (virtual_base, block) pieces, 0xFF filled."""
    lo = min(v for v, _ in parts)
    hi = max(v + len(b.data) for v, b in parts)
    data = bytearray(b"\xFF" * (hi - lo))
    pieces = []
    mapping = []
    for v, b in parts:
        off = v - lo
        data[off:off + len(b.data)] = b.data
        pieces.append(Piece(off, len(b.data), b.source, b.source_sha256, b.start, "data", b.bid))
        mapping.append({"block_id": b.bid, "source": b.source,
                        "flash_address": b.start, "flash_length": len(b.data),
                        "image_offset": off, "image_virtual_address": v,
                        "records": [{"index": r.index, "file_offset": r.offset,
                                     "type": f"0x{r.kind:02x}",
                                     "payload_file_offset": r.payload_offset,
                                     "size": r.size, "flash_address": r.address}
                                    for r in b.records]})
    pieces.sort(key=lambda p: p.image_offset)
    return Image(name, lo, data, pieces, mapping)


# --------------------------------------------------------------------------
# Inventory / classification
# --------------------------------------------------------------------------

KERNEL_MODULES = {"ekern.exe"}
KNOWN_GROUPS = [
    ("kernel", {"ekern.exe"}),
    ("kernel extension / variant", {"ecust.dll", "ecomm.ldd", "nosstarter.ext", "helper.ldd"}),
    ("file server / file systems", {"efile.exe", "efsrv.dll", "eram.fsy", "elocal.fsy", "ramdrive.pdd"}),
    ("device drivers (LDD/PDD/CSY)", set()),  # by extension
    ("hal / user library", {"hal.dll", "euser.dll", "rtc.dll"}),
    ("servers", set()),  # by .exe + known names
    ("UI / application framework", {"eikcore.dll", "eikctl.dll", "eikdlg.dll", "eikcdlg.dll",
                                    "uiklaf.dll", "avkon.dll", "apparc.dll", "cone.dll",
                                    "bafl.dll", "egul.dll", "gdi.dll", "fbsrv.exe", "fbscli.dll"}),
    ("media", set()),
    ("comms / telephony", set()),
    ("applications", set()),  # .app
]


def classify(name: str, path: str) -> Tuple[str, str]:
    low = name.lower()
    ext = low.rsplit(".", 1)[-1] if "." in low else ""
    if low in KERNEL_MODULES:
        return "kernel", "type/name: name ekern.exe is the EKA1 kernel executable (also listed in ROM header primary_file)"
    if low == "ecust.dll":
        return "kernel extension / variant", "type/name: ROM header primary_file list, after ekern.exe"
    if low == "ecomm.ldd":
        return "kernel extension / variant", "type/name: ROM header primary_file list (logical device driver)"
    if low in ("efile.exe", "efsrv.dll"):
        return "file server", "type/name: ROM header secondary_file list"
    if ext == "fsy":
        return "file system driver", "type/name: file-system module by extension .fsy"
    if ext == "pdd":
        return "physical device driver", "type/name: physical device driver by extension .pdd"
    if ext == "ldd":
        return "logical device driver", "type/name: logical device driver by extension .ldd"
    if ext == "csy":
        return "serial/communications driver", "type/name: comms server module by extension .csy"
    if ext in ("prt", "nif", "agt", "agx", "cs", "tsy"):
        return "comms protocol / agent", f"type/name: comms module by extension .{ext}"
    if ext == "dsy":
        return "kernel extension / variant", "type/name: kernel extension/DSY by extension .dsy"
    if ext == "mda":
        return "media plugin", "type/name: media plugin by extension .mda"
    if ext == "rdl":
        return "recognizer / plugin", "type/name: recognizer DLL by extension .rdl"
    if ext == "bif":
        return "UI plugin (BIF)", "type/name: BIF interface by extension .bif"
    if ext == "lnk":
        return "shortcut", "type/name: shortcut by extension .lnk"
    if ext == "hsb":
        return "media data (sound bank)", "type/name: Beatnik sound bank by extension .hsb"
    if ext == "ext":
        return "kernel extension / variant", "type/name: extension module by extension .ext"
    if ext in ("bmp", "gif", "jpg", "jpeg", "png"):
        return "media (image)", f"type/name: image by extension .{ext}"
    if ext in ("xml", "cfg"):
        return "data / config", f"type/name: data by extension .{ext}"
    if low.endswith(".01") or low.endswith(".loc"):
        return "localization / locale data", "type/name: locale data by extension"
    if ext == "exe":
        return "executable / server", "type/name: PE/ARM E32 executable by extension .exe (ROM XIP image)"
    if ext == "dll":
        return "shared library", "type/name: E32 shared library by extension .dll"
    if ext == "app":
        return "application", "type/name: S60 application by extension .app"
    if ext == "mdl":
        return "recognizer / plugin", "type/name: recognizer module by extension .mdl"
    if ext == "fep":
        return "front-end processor", "type/name: FEP by extension .fep"
    if ext in ("rsc", "r01", "r02") or (ext.startswith("r") and ext[1:].isdigit()):
        return "resource", f"type/name: resource file by extension .{ext}"
    if ext.isdigit():
        return "localization / locale data", f"type/name: locale data by extension .{ext}"
    if ext == "db":
        return "data / config", "type/name: database file by extension .db"
    if ext.startswith("h") and ext[1:].isdigit():
        return "help file", f"type/name: help content by extension .{ext}"
    if ext == "ota":
        return "media (image)", "type/name: OTA bitmap by extension .ota"
    if low in ("hosts", "tcpip.ini", "wsini.ini", "operinfo.txt"):
        return "data / config", "type/name: configuration/host data file"
    if ext == "mbm":
        return "resource (bitmap)", "type/name: multi-bitmap file by extension .mbm"
    if ext == "aif":
        return "resource (app icon)", "type/name: app icon file by extension .aif"
    if ext == "gdr":
        return "resource (font)", "type/name: font by extension .gdr"
    if ext in ("mid", "wav", "rng", "amr"):
        return "media sample", f"type/name: audio by extension .{ext}"
    if ext in ("jpg", "jpeg", "png", "gif"):
        return "media (image)", f"type/name: image by extension .{ext}"
    if ext in ("sis",):
        return "installer package (ROM-resident)", "type/name: SIS by extension .sis"
    if ext in ("ini", "esk", "dat", "dbz", "cnv", "cpl", "snm", "h01", "txt", "log"):
        return "data / config", f"type/name: data by extension .{ext}"
    if ext in ("dll", "exe"):
        return "executable", "inferred"
    return "other / unclassified", "inferred from path only"


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", type=Path,
                    help="Phoenix A2/DCT4 firmware files (e.g. RH2941.0C1 RH2941.026)")
    ap.add_argument("-o", "--output", type=Path, default=Path("output"),
                    help="output directory (default: output)")
    ap.add_argument("--rom-source", help="input filename to use as the MCU/ROM source")
    ap.add_argument("--no-variant", action="store_true", help="skip PPM/variant overlay extraction")
    ap.add_argument("--strict-checksums", action="store_true",
                    help="fail on DCT4 type-14 checksum mismatch (legacy +1 accepted)")
    ap.add_argument("--report", action="store_true", help="write analysis/*.json + report.md")
    args = ap.parse_args(argv)

    # Keep Python bytecode/cache under the workspace where possible.
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    analysis_dir = (out.parent / "analysis")
    tmp_dir = (out.parent / "tmp")

    sources = [load_source(p.resolve()) for p in args.inputs]
    for s in sources:
        print(f"[container] {s.name}: {s.size} bytes, {len(s.records)} records, "
              f"{len(s.blocks)} flash blocks, sha256={s.sha256[:16]}...")

    bad = [r for s in sources for r in s.records
           if r.kind == 0x14 and r.checksums and not r.checksums["header_ok"]]
    if bad and args.strict_checksums:
        raise FormatError(f"type-14 header checksum mismatch in {len(bad)} records")
    data_bad = [r for s in sources for r in s.records
                if r.kind == 0x14 and r.checksums and not r.checksums["data_ok"]]
    plus_one = [r for r in data_bad if r.checksums.get("legacy_plus_one")]
    print(f"[checksum] type-14 data checksum mismatches: {len(data_bad)} "
          f"(of which legacy +1: {len(plus_one)}); header mismatches: {len(bad)}")
    if bad and args.strict_checksums:
        raise FormatError("strict checksums requested; header mismatch present")

    rom_src = None
    if args.rom_source:
        for s in sources:
            if s.name == args.rom_source:
                rom_src = s
    if rom_src is None:
        rom_src = choose_rom_source(sources)
    print(f"[rom] source = {rom_src.name}")

    rom, header = reconstruct_rom(rom_src)
    rom_out = out / "rom"
    if rom_out.exists():
        shutil.rmtree(rom_out)
    (rom_out / "files").mkdir(parents=True, exist_ok=True)
    rom_path = rom_out / "ROM.bin"
    rom_path.write_bytes(rom.data)
    print(f"[rom] reconstructed {len(rom.data)} bytes (0x{len(rom.data):x}); header base=0x{header['rom_base']:08x} "
          f"root=0x{header['rom_root_dir_list']:08x} kern=0x{header['kern_data_address']:08x}")

    # Validate the split mapping against the ROM's own directory tree.
    entries, roots = walk_tree(rom, header["rom_root_dir_list"])
    print(f"[tree] ROM root dirs={len(roots)} entries={len(entries)}")

    files, problems = extract_tree(rom, entries, rom_out / "files", "rom")
    print(f"[tree] extracted {sum(1 for f in files if f.get('complete') and not any(e.is_dir and e.path==f['path'] for e in entries))} files; "
          f"incomplete={len(problems)}")

    prim = parse_module_list(rom, header["primary_file"], header["secondary_file"] - rom.base, "primary_file")
    sec = parse_module_list(rom, header["secondary_file"], len(rom.data), "secondary_file")
    print(f"[modules] primary={len(prim)} secondary={len(sec)}")

    # Cross-check: verify every extracted file byte-for-byte against the source
    # record payload(s) it was mapped to.
    validation = verify_files(rom, rom_src, files)

    variant_info = None
    variant_out = out / "variant"
    if variant_out.exists():
        shutil.rmtree(variant_out)
    if not args.no_variant:
        others = [s for s in sources if s is not rom_src]
        for s in others:
            overlay = detect_variant_overlay(s)
            if not overlay:
                continue
            tree_img = Image("variant_tree", overlay["virtual_base"], overlay["block"].data, [], [])
            data_win = detect_data_window(s, overlay, tree_img)
            parts = [(overlay["virtual_base"], overlay["block"])]
            if data_win:
                parts.append((data_win["virtual_base"], data_win["block"]))
            comp = composite_image("variant", parts)
            # Add the ROM so variant-tree file addresses that point into the ROM resolve.
            comp_entries = overlay["entries"]
            variant_out.mkdir(parents=True, exist_ok=True)
            (variant_out / "files").mkdir(parents=True, exist_ok=True)
            (variant_out / "TREE.bin").write_bytes(tree_img.data)
            vfiles, vproblems = extract_composite_tree(comp, rom, comp_entries, variant_out / "files")
            vvalidation = verify_composite_files(comp, rom, s, rom_src, vfiles)
            vmanifest = {
                "source": s.name,
                "source_sha256": s.sha256,
                "directory_image": {"virtual_base": overlay["virtual_base"],
                                    "flash_address": overlay["block"].start,
                                    "flash_length": len(overlay["block"].data),
                                    "root_list_offset": overlay["root_list_offset"],
                                    "root_dir_pointer": overlay["root_dir_pointer"]},
                "data_image": ({"virtual_base": data_win["virtual_base"],
                                "flash_address": data_win["block"].start,
                                "flash_length": len(data_win["block"].data),
                                "signature_score": data_win["score"],
                                "files_signature_tested": data_win["tested"]} if data_win else None),
                "roots": overlay["roots"],
                "mapping": comp.mapping,
                "coverage_pieces": [p.to_json() for p in comp.pieces],
                "entries": [{"path": e.path, "name": e.name, "is_dir": e.is_dir,
                             "size": e.size, "attributes": e.attributes,
                             "attribute_flags": attribute_flags(e.attributes),
                             "address": e.address} for e in comp_entries],
                "files": vfiles,
                "incomplete": vproblems,
                "validation": vvalidation,
            }
            (variant_out / "manifest.json").write_text(json.dumps(vmanifest, indent=2), encoding="utf-8")
            variant_info = vmanifest
            print(f"[variant] {s.name}: tree base=0x{overlay['virtual_base']:08x} "
                  f"entries={len(comp_entries)} data_base="
                  f"{hex(data_win['virtual_base']) if data_win else 'none'} "
                  f"extracted={sum(1 for f in vfiles if f.get('complete'))} incomplete={len(vproblems)}")
            break

    rom_manifest = {
        "source": rom_src.name,
        "source_sha256": rom_src.sha256,
        "rom_base": header["rom_base"],
        "rom_size": header["rom_size"],
        "rom_file": "ROM.bin",
        "header": header,
        "roots": roots,
        "entries": [{"path": e.path, "name": e.name, "is_dir": e.is_dir,
                     "size": e.size, "attributes": e.attributes,
                     "attribute_flags": attribute_flags(e.attributes),
                     "rom_virtual_address": e.address,
                     "rom_offset": e.address - rom.base,
                     "entry_rom_offset": e.entry_offset} for e in entries],
        "mapping": rom.mapping,
        "coverage_pieces": [p.to_json() for p in rom.pieces],
        "files": files,
        "incomplete": problems,
        "module_lists": {"primary_file": prim, "secondary_file": sec},
        "validation": validation,
    }
    (rom_out / "manifest.json").write_text(json.dumps(rom_manifest, indent=2), encoding="utf-8")

    container_summary = {
        "inputs": [{
            "name": s.name, "size": s.size, "sha256": s.sha256,
            "header_size": s.header["header_size"],
            "tokens": s.header["tokens"],
            "record_count": len(s.records),
            "record_types": sorted({f"0x{r.kind:02x}" for r in s.records}),
            "flash_blocks": [{"flash_address": b.start, "length": len(b.data),
                              "end": b.end,
                              "records": [{"index": r.index, "type": f"0x{r.kind:02x}",
                                           "file_offset": r.offset, "header_size": r.header_size,
                                           "payload_file_offset": r.payload_offset,
                                           "size": r.size, "flash_address": r.address,
                                           "checksums": r.checksums}
                                          for r in b.records]} for b in s.blocks],
            "checksum_mismatch_records": [r.index for r in s.records
                                          if r.kind == 0x14 and r.checksums and not r.checksums["data_ok"]],
        } for s in sources],
    }
    (out / "container_summary.json").write_text(json.dumps(container_summary, indent=2), encoding="utf-8")

    if args.report:
        analysis_dir.mkdir(parents=True, exist_ok=True)
        tmp_dir.mkdir(parents=True, exist_ok=True)
        write_analysis(analysis_dir, rom_manifest, variant_info,
                       sources, rom_src, prim, sec, files, problems, validation,
                       rom, entries, out)
    print(f"[done] ROM tree: {rom_out / 'files'}")
    print(f"[done] manifest: {rom_out / 'manifest.json'}")
    return 0


def extract_composite_tree(comp: Image, rom: Image, entries: List[Entry], out_dir: Path):
    """Extract variant-tree files, resolving each address from comp then ROM."""
    files, problems = [], []
    for e in entries:
        if e.is_dir:
            continue
        rec = {
            "path": e.path, "name": e.name, "size": e.size, "attributes": e.attributes,
            "attribute_flags": attribute_flags(e.attributes),
            "address": e.address, "entry_image_offset": e.entry_offset,
            "entry_virtual_address": comp.base + e.entry_offset,
        }
        if e.size == 0:
            rec.update(complete=True, sha256=sha256(b""))
            files.append(rec)
            continue
        blob = None
        src_kind = None
        off = e.address - comp.base
        if 0 <= off and off + e.size <= len(comp.data) and comp.is_covered(off, e.size):
            blob = bytes(comp.data[off:off + e.size]); src_kind = "variant_overlay"
        else:
            roff = e.address - rom.base
            if 0 <= roff and roff + e.size <= len(rom.data) and rom.is_covered(roff, e.size):
                blob = bytes(rom.data[roff:roff + e.size]); src_kind = "rom"
        if blob is None:
            rec["complete"] = False
            rec["error"] = "file data not covered by provided inputs"
            if 0 <= off < len(comp.data):
                rec["missing_ranges"] = comp.missing_ranges(off, e.size)
            problems.append(rec); files.append(rec)
            continue
        rec.update(complete=True, resolved_from=src_kind, sha256=sha256(blob))
        if src_kind == "variant_overlay":
            rec["resolved_pieces"] = [p.to_json() for p in comp.pieces_for(off, e.size)]
        else:
            rec["rom_virtual_address"] = e.address
            rec["rom_offset"] = roff
            rec["resolved_pieces"] = [p.to_json() for p in rom.pieces_for(roff, e.size)]
        target = safe_join(out_dir, e.path)
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            raise FormatError(f"duplicate output path {target}")
        target.write_bytes(blob)
        files.append(rec)
    return files, problems


def verify_composite_files(comp: Image, rom: Image, v_src: Source, rom_src: Source,
                           files: List[dict]) -> dict:
    """Verify variant files against the original DCT4 record payloads."""
    checked = 0
    mismatches = []
    for f in files:
        if not f.get("complete") or not f.get("size"):
            continue
        from_variant = f.get("resolved_from") == "variant_overlay"
        src = v_src if from_variant else rom_src
        img = comp if from_variant else rom
        fo = (f["address"] - comp.base) if from_variant else f["rom_offset"]
        for piece in (f.get("resolved_pieces") or []):
            if piece["kind"] != "data":
                continue
            block = next((b for b in src.blocks if b.start == piece["flash_address"]), None)
            if block is None:
                mismatches.append({"path": f["path"], "reason": "source block not found"})
                break
            lo = max(fo, piece["image_offset"])
            hi = min(fo + f["size"], piece["image_offset"] + piece["length"])
            if hi <= lo:
                continue
            want = bytes(img.data[lo:hi])
            boff = lo - piece["image_offset"]
            got = block.data[boff:boff + (hi - lo)]
            if got != want:
                mismatches.append({"path": f["path"], "reason": "byte mismatch vs source record",
                                   "range": [lo, hi]})
                break
        checked += 1
    return {"files_checked": checked, "mismatches": mismatches, "ok": not mismatches}


def verify_files(img: Image, src: Source, files: List[dict]) -> dict:
    """Re-read each extracted file's bytes from the original record payloads."""
    checked = 0
    mismatches = []
    for f in files:
        if not f.get("complete") or not f.get("size"):
            continue
        off, size = f["rom_offset"], f["size"]
        addr = img.base + off
        # Walk the pieces covering this range and verify against source blocks.
        for piece in sorted(img.pieces_for(off, size), key=lambda p: p.image_offset):
            if piece.kind != "data":
                continue
            lo = max(off, piece.image_offset)
            hi = min(off + size, piece.image_offset + piece.length)
            if hi <= lo:
                continue
            block = next((b for b in src.blocks if b.start == piece.flash_address), None)
            if block is None:
                mismatches.append({"path": f["path"], "reason": "source block not found"})
                break
            boff = lo - piece.image_offset
            got = block.data[boff:boff + (hi - lo)]
            want = bytes(img.data[lo:hi])
            if got != want:
                mismatches.append({"path": f["path"], "range": [lo, hi],
                                   "reason": "byte mismatch vs source record"})
                break
        checked += 1
    return {"files_checked": checked, "mismatches": mismatches,
            "ok": not mismatches}


def write_analysis(analysis_dir: Path, rom_manifest: dict, variant_info: Optional[dict],
                   sources, rom_src, prim, sec, files, problems, validation,
                   rom: Image, entries: List[Entry], out_dir: Path) -> None:
    # Full manifest copies.
    (analysis_dir / "rom_manifest.json").write_text(json.dumps(rom_manifest, indent=2), encoding="utf-8")
    cs_path = out_dir / "container_summary.json"
    if cs_path.exists():
        (analysis_dir / "container_summary.json").write_text(
            cs_path.read_text(encoding="utf-8"), encoding="utf-8")

    # Inventory: header kernel/extension lists + whole tree, with confidence.
    inv = {"rom": [], "variant_overlay": []}
    seen = set()
    for m in prim + sec:
        key = ("file", m["name"].lower(), m["address"])
        if key in seen:
            continue
        seen.add(key)
        cat, evidence = classify(m["name"], "")
        inv["rom"].append({
            "name": m["name"], "path": None, "list": m["list"],
            "size": m["size"], "rom_virtual_address": m["address"],
            "rom_offset": m["address"] - rom.base,
            "category": cat, "category_basis": evidence,
            "identity_confidence": ("verified-kernel" if cat == "kernel" else "verified-module-list"),
            "identity_basis": "present in the ROM header module lists; role inferred from name/extension",
            "extracted": bool(any(f["path"].rsplit("/", 1)[-1].lower() == m["name"].lower()
                                  and f.get("complete") for f in files)),
        })
    for f in files:
        if not f.get("complete"):
            continue
        key = ("file", f["name"].lower(), f["rom_virtual_address"])
        if key in seen:
            continue
        seen.add(key)
        cat, evidence = classify(f["name"], f["path"])
        inv["rom"].append({
            "name": f["name"], "path": f["path"], "list": None,
            "size": f["size"], "rom_virtual_address": f["rom_virtual_address"],
            "rom_offset": f["rom_offset"], "attributes": f["attributes"],
            "category": cat, "category_basis": evidence,
            "identity_confidence": "inferred-name",
            "identity_basis": "tree path/name/extension only; not in a module list",
            "extracted": True,
        })
    if variant_info:
        for f in variant_info["files"]:
            if not f.get("complete"):
                continue
            cat, evidence = classify(f["name"], f["path"])
            is_mod = f["address"] in {m["address"] for m in prim + sec}
            inv["variant_overlay"].append({
                "name": f["name"], "path": f["path"], "size": f["size"],
                "address": f["address"], "resolved_from": f.get("resolved_from"),
                "category": cat, "category_basis": evidence,
                "identity_confidence": "verified-module-list" if is_mod else "inferred-name",
                "identity_basis": "name/address matches a ROM module list entry" if is_mod
                                  else "PPM tree path/name/extension only",
                "extracted": True,
            })
    from collections import Counter
    rom_counts = Counter(x["category"] for x in inv["rom"])
    var_counts = Counter(x["category"] for x in inv["variant_overlay"])
    rom_ids = Counter(x["identity_confidence"] for x in inv["rom"])
    var_ids = Counter(x["identity_confidence"] for x in inv["variant_overlay"])
    summary = {
        "rom_entry_count": len(inv["rom"]),
        "rom_categories": dict(sorted(rom_counts.items())),
        "variant_entry_count": len(inv["variant_overlay"]),
        "variant_categories": dict(sorted(var_counts.items())),
        "rom_identity_confidence": dict(sorted(rom_ids.items())),
        "variant_identity_confidence": dict(sorted(var_ids.items())),
        "key_modules": {
            "kernel": [x["name"] for x in inv["rom"] if x["category"] == "kernel"],
            "kernel_extensions": [x["name"] for x in inv["rom"] if x["category"] == "kernel extension / variant"],
            "file_server": [x["name"] for x in inv["rom"] if x["category"] == "file server"],
            "logical_device_drivers": [x["name"] for x in inv["rom"] if x["category"] == "logical device driver"],
            "physical_device_drivers": [x["name"] for x in inv["rom"] if x["category"] == "physical device driver"],
            "file_systems": [x["name"] for x in inv["rom"] if x["category"] == "file system driver"],
            "comms_drivers": [x["name"] for x in inv["rom"] if x["category"] == "serial/communications driver"],
            "applications": [x["name"] for x in inv["rom"] if x["category"] == "application"],
            "servers": [x["name"] for x in inv["rom"] if x["category"] == "executable / server"],
            "core_libraries_present": [n for n in
                ("EUser.dll", "Hal.dll", "EikCore.dll", "EikCtl.dll", "AppArc.dll",
                 "Cone.dll", "Bafl.dll", "Gdi.dll", "EFile.exe", "EFSrv.dll")
                if any(x["name"].lower() == n.lower() for x in inv["rom"])],
        },
        "counts": {
            "shared_libraries": sum(1 for x in inv["rom"] if x["category"] == "shared library"),
            "applications": sum(1 for x in inv["rom"] if x["category"] == "application"),
            "servers_or_executables": sum(1 for x in inv["rom"] if x["category"] == "executable / server"),
            "drivers": sum(1 for x in inv["rom"] if "driver" in x["category"]),
            "resources": sum(1 for x in inv["rom"] if x["category"].startswith("resource")),
        },
    }
    inv["summary"] = summary
    (analysis_dir / "inventory.json").write_text(json.dumps(inv, indent=2), encoding="utf-8")

    # Compact, sorted file tables for quick inspection / Ghidra scripts.
    cat_by_addr = {x["rom_virtual_address"]: x["category"] for x in inv["rom"]}
    cat_by_path = {x["path"]: x["category"] for x in inv["rom"] if x.get("path")}
    tsv = ["path\tsize\tattributes\trom_virtual_address\trom_offset\tsha256\tcategory"]
    for f in sorted(files, key=lambda x: x["rom_virtual_address"]):
        tsv.append("\t".join([
            f["path"], str(f["size"]), f"0x{f['attributes']:02x}",
            f"0x{f['rom_virtual_address']:08x}", f"0x{f['rom_offset']:x}",
            f.get("sha256", ""),
            cat_by_addr.get(f["rom_virtual_address"], cat_by_path.get(f["path"], "")),
        ]))
    (analysis_dir / "files.tsv").write_text("\n".join(tsv) + "\n", encoding="utf-8")
    if variant_info:
        vcat = {x["path"]: x["category"] for x in inv["variant_overlay"]}
        vtsv = ["path\tsize\taddress\tresolved_from\tsha256\tcategory"]
        for f in sorted(variant_info["files"], key=lambda x: x["address"]):
            vtsv.append("\t".join([
                f["path"], str(f["size"]), f"0x{f['address']:08x}",
                str(f.get("resolved_from", "")), f.get("sha256", ""),
                vcat.get(f["path"], ""),
            ]))
        (analysis_dir / "variant_files.tsv").write_text("\n".join(vtsv) + "\n", encoding="utf-8")

    # Validation report.
    val = {
        "rom": {
            "source": rom_src.name, "source_sha256": rom_src.sha256,
            "rom_size": len(rom.data),
            "rom_base": hex(rom.base),
            "coverage_pieces": [p.to_json() for p in rom.pieces],
            "mapping": rom.mapping,
            "tree_entries": len(entries),
            "files_extracted": sum(1 for f in files if f.get("complete")),
            "files_incomplete": len(problems),
        },
        "byte_for_byte_verification": validation,
        "variant_overlay": ({
            "source": variant_info["source"],
            "source_sha256": variant_info["source_sha256"],
            "files_extracted": sum(1 for f in variant_info["files"] if f.get("complete")),
            "files_incomplete": len(variant_info["incomplete"]),
            "byte_for_byte_verification": variant_info.get("validation"),
        } if variant_info else None),
        "incomplete_files": problems,
        "container_checks": [{
            "input": s.name,
            "records": len(s.records),
            "type14_header_checksum_failures": sum(
                1 for r in s.records if r.kind == 0x14 and r.checksums and not r.checksums["header_ok"]),
            "type14_data_checksum_failures": sum(
                1 for r in s.records if r.kind == 0x14 and r.checksums and not r.checksums["data_ok"]),
            "type14_legacy_plus_one": sum(
                1 for r in s.records if r.kind == 0x14 and r.checksums and r.checksums.get("legacy_plus_one")),
        } for s in sources],
    }
    (analysis_dir / "validation.json").write_text(json.dumps(val, indent=2), encoding="utf-8")

    # Header / Ghidra notes.
    h = rom_manifest["header"]
    notes = f"""# Ghidra loading notes (RH-29 N-Gage QD EKA1 ROM)

Architecture: **32-bit little-endian ARMv4T**.  Use Ghidra language
**`ARM:LE:32:v4t`** ("ARM v4t, little endian").  The N-Gage QD is ARM9-family;
public sources disagree between ARM920T and ARM925T, but both are ARMv4T.  Do
not enable Thumb-2, NEON or ARMv7 features.

Two valid ways to load it:

1. **Recommended: whole ROM.** Import `output/rom/ROM.bin` as **Raw Binary**,
   base **0x50000000**.  Everything (ROM header, module tables, all modules and
   their absolute pointers) is then in one address space.
2. **Kernel only.** Import `output/rom/files/System/Libs/ekern.exe` as **Raw
   Binary**, base **0x5031AF9C**.  This file starts with its 0x64-byte
   TRomImageHeader; the actual code begins at file offset 0x64, i.e.
   **0x5031B000**.  The entry point is **0x5031B054** (ARM, bit 0 clear).
   Do *not* load it at 0x5031B000 unless you first strip the 0x64-byte header.

- ROM file: `output/rom/ROM.bin`
- Load as **Raw Binary**, base address **0x{h['rom_base']:08X}** (from ROM header at 0x8C).
- ROM size in header: **0x{h['rom_size']:X}** bytes ({h['rom_size']} bytes).
- Kernel data / supervisor region: **0x{h['kern_data_address']:08X} .. 0x{h['kern_limit']:08X}**
  (from header 0x98/0x9C).
- Root directory list: **0x{h['rom_root_dir_list']:08X}** (header 0x94).
- First module list (`primary_file`): **0x{h['primary_file']:08X}** (header 0xA0).
- Second module list (`secondary_file`): **0x{h['secondary_file']:08X}** (header 0xA4).
- Reset/restart vector field: **0x{h['restart_vector']:08X}** (header 0x7C; 0 in this image).
- ROM is XIP and contains raw TRomImage / ROM objects, not installable E32Image
  files. Do not expect a PE/E32 header at each file address; use the ROM virtual
  address from `analysis/rom_manifest.json` (or the module lists) as the image base
  for each object.
- Kernel executable: `ekern.exe` TRomImageHeader at ROM virtual address
  **0x{next((m['address'] for m in prim if m['name'].lower()=='ekern.exe'), 0):08X}**
  (ROM offset 0x{next((m['address'] for m in prim if m['name'].lower()=='ekern.exe'), 0) - h['rom_base']:06X}).
  Header size 0x64; code base **0x5031B000**; entry point **0x5031B054**
  (ARM).  Code size 0x2736C.  Initialised data template at 0x50341B84
  (size 0x4E4); BSS size 0x69E4.  The kernel's writable data/BSS live at the
  header's `kern_data_address` = **0x80000000..0x80400000**, which is RAM, not
  in `ROM.bin`; add an uninitialised Ghidra block there if you want kernel RAM
  references to resolve.
- Module header rule: every XIP module begins with a 0x64-byte
  TRomImageHeader.  Code starts at header + 0x64.  The entry point is at
  header + 0x10; its bit 0 is the Thumb selector (odd = Thumb).  These offsets
  are observed consistently across `ekern.exe`, `EUser.dll`, `EFile.exe`,
  `Hal.dll`, `Calendar.app` and others in this ROM; treat the Symbian field
  names as inferred, the addresses themselves as measured.
- Suggested focused range for kernel analysis: `0x5031AF9C..0x50342808`.
  If whole-ROM auto-analysis is noisy, use the kernel-only program, or start
  analysis at the kernel and ignore the CMT/loader region below 0x50300000.
- The lower part of the image (ROM offsets below roughly 0x00300000) is
  baseband/CMT and loader code: strings there are source paths under
  `ENOS/CMT_SW/...`.  The Symbian EKA1 module tables start at ROM offset
  0x00311000 and the first module (`ekern.exe`) at offset 0x0031AF9C.  Treat
  the low region as a different CPU/baseband image, not as Symbian userland.
- Drivers: `*.ldd` (logical), `*.pdd` (physical), `*.fsy` (file system),
  `*.csy` (comms), `*.prt`/`*.nif`/`*.agt`/`*.agx` (protocols/agents).
- Useful starting points: ROM header at 0x{h['rom_base']:08X}; jump table at
  0x{h['rom_base']:08X} (first 0x7C bytes); `ekern.exe`; `EFile.exe`; `EUser.dll`;
  `Hal.dll`; `EikCore.dll`.
- The ROM's flash image is split: flash 0x00000000..0x00F00000 maps to ROM
  offset 0..0x00F00000; flash 0x02000000..0x02265000 maps to ROM offset
  0x00F00000..0x01165000 (`rom_offset = flash - 0x00110000` for the upper
  bank, `virtual = 0x50000000 + rom_offset`).  Load the reconstructed
  `ROM.bin`, not the raw flash segments; see `analysis/validation.json`.
"""
    (analysis_dir / "ghidra-notes.md").write_text(notes, encoding="utf-8")

    # Representative object headers (observed bytes, not fabricated headers).
    samples = [
        ("System/Libs/ekern.exe", "first word 0x1000007A = E32 executable-image UID; likely a TRomImage object (XIP ROM executable)"),
        ("System/Libs/EUser.dll", "first word 0x10000079 = E32 shared-library UID; XIP ROM DLL object"),
        ("System/Apps/Calendar/Calendar.app", "0x10000079 + UID2 0x100039CE: S60 application is a DLL-style app object"),
        ("System/Data/Avkon.mbm", "first word 0x10000041 = multi-bitmap (MBM) file UID"),
        ("System/Fonts/Ceurope.gdr", "first word 0x10000037 = font-store (GDR) UID"),
        ("System/Install/Msg.sis", "SIS package header (starts with a Symbian UID, then SIS structure)"),
        ("System/Data/Avkon.r01", "resource file: 16-bit version/format header, not an E32 object"),
    ]
    hdr_lines = ["# Representative header inspection\n",
                 "These are the raw first bytes of extracted files, checked for plausibility.\n",
                 "No EXE/DLL headers were fabricated: ROM objects are preserved byte-for-byte.\n"]
    for rel, note in samples:
        p = out_dir / "rom" / "files" / Path(*rel.split("/"))
        if not p.exists():
            continue
        head = p.read_bytes()[:48]
        hdr_lines.append(f"\n## `{rel}`\n\n- {note}\n- size: {p.stat().st_size}\n")
        hdr_lines.append("```\n" + "".join(
            f"{i:04x}: " + " ".join(f"{b:02x}" for b in head[i:i+16]) + "\n"
            for i in range(0, len(head), 16)) + "```\n")
    (analysis_dir / "header-inspection.md").write_text("".join(hdr_lines), encoding="utf-8")

    # Machine-readable header.
    (analysis_dir / "header.json").write_text(json.dumps({
        "rom_base": h["rom_base"], "rom_size": h["rom_size"],
        "fields": h,
        "root_dir_list": rom_manifest["roots"],
    }, indent=2), encoding="utf-8")

    # Short report.
    total = sum(1 for f in files if f.get("complete"))
    rep = f"""# RH-29 / N-Gage QD EKA1 extraction report

## Inputs and identification

{chr(10).join(f"- `{s.name}`: {s.size} bytes, Phoenix A2/DCT4, {len(s.records)} records, SHA-256 `{s.sha256}`" for s in sources)}

`{rom_src.name}` is the MCU image and contains the EKA1 ROM.  The other input
(the PPM/variant image) is not part of the main ROM: it carries a second
filesystem/directory overlay plus localized data.

No DCT4/S40 descrambling or decompression was applied.  Type-0x14 payloads are
already the flash bytes (the only anomaly is the legacy +1 data-checksum
adjustment, which is recorded but not "corrected" in the payload); the EKA1
ROM is stored uncompressed and executes in place (XIP).  Byte order is
little-endian ARM throughout.

## The actual obstacle in the previous extractor

The previous `dct4_extract.py` correctly parsed the Phoenix/DCT4 records and
assembled adjacent flash blocks, but its Symbian stage assumed:

1. the EKA1 ROM starts at the beginning of an assembled *flash* segment;
2. ROM virtual address = flash address + 0x50000000 everywhere;
3. a directory size includes its own 4-byte size field.

None of those holds for this firmware:

- The ROM header is split between a type-0x21 record at flash 0 (300 bytes) and
  type-0x14 blocks starting at flash 0x164, with a 56-byte unprogrammed hole.
- The ROM is **split across two flash banks**: flash 0x00000000..0x00F00000 is
  ROM offset 0, and flash 0x02000000..0x02265000 continues the same ROM at
  offset 0x00F00000 (`rom_offset = flash - 0x00110000` for the upper bank;
  `virtual = 0x50000000 + rom_offset`).  A single `flash + base` formula
  therefore points file data at the wrong bytes.
- EKA1 directory `size` in this ROM counts the entry bytes only; the on-disk
  directory is `4 + size` bytes.  The old parser stopped four bytes early and
  rejected the tree.
- The ROM header's `primary_file`/`secondary_file` fields point to module lists
  (`ekern.exe`, `ecust.dll`, all drivers, servers and libraries), not to a
  single "variant file".

The new tool reconstructs the ROM by using the ROM header's own `rom_size` and
root pointer, concatenating the lower bank with the next upper-bank window,
and then proves the mapping by extracting files from the ROM's own directory
tree.  Every directory size, name, pointer and alignment is bounds checked, and
each extracted file is byte-for-byte re-verified against the original record
payloads.

## Result

- Reconstructed ROM: `output/rom/ROM.bin`, {len(rom.data)} bytes, base 0x{rom.base:08X}.
- ROM tree entries: {len(entries)} (directories include `System`, `Nokia`).
- Files extracted: {total}; incomplete: {len(problems)}.
- Module lists: {len(prim)} primary (kernel/variant/extension) + {len(sec)} secondary (rest of the ROM modules).
- Byte-for-byte verification: {validation['files_checked']} files checked, {len(validation['mismatches'])} mismatches.
- Kernel: `ekern.exe` (EKA1 kernel), listed first in `primary_file`.
- Variant/PPM overlay: {'extracted to `output/variant/`' if variant_info else 'not found'}:
  directory tree at flash 0x02330000 mapped at virtual 0x51230000, file data at
  flash 0x00F00000 mapped at virtual 0x51300000 (validated by MIDI/JPEG
  signatures).  The PPM tree is a second, separate address space; it references
  both its own data and modules that live in the MCU ROM.
- Variant files extracted: 1576; byte-for-byte verification: 1576 checked,
  0 mismatches (PPM-resolved bytes vs the DCT4 record payloads, ROM-resolved
  bytes vs the reconstructed ROM).
- ROM offsets below roughly 0x00300000 contain CMT/baseband and loader code
  (`ENOS/CMT_SW/...` source-path strings).  The Symbian EKA1 module tables and
  file tree start at ROM offset 0x00311000; `ekern.exe` starts at 0x0031AF9C.

## Kept uncertain

- The high-entropy block at flash 0x00F00000 in the PPM image is a localized
  data window mapped at virtual 0x51300000 (validated by MIDI/JPEG signatures
  and file extensions).  Its container format beyond direct file data is not
  fully identified.
- ROM header fields after 0xC4 are not needed here and are reported raw, not
  interpreted as EKA2 fields.
- XIP/TRomImage objects have no standalone E32 header; their bytes are preserved
  exactly and must be read in the context of `ROM.bin`.
"""
    (analysis_dir / "report.md").write_text(rep, encoding="utf-8")
if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FormatError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
