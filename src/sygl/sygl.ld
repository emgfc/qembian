/* sygl blob: fixed ROM address, read-only (docs/gpu-roadmap.md H-gles-blob).
   SYGL_BASE comes from build.sh (--defsym). The entry table must stay first: the libgles_cm
   patches (models/<id>/gl/gles-patches.txt) branch to SYGL_BASE + 4 * (ordinal - 1). */
ENTRY(sygl_entry_table)
SECTIONS
{
	. = SYGL_BASE;
	.text : {
		KEEP(*(.text.entry))
		*(.text .text.*)
		*(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
	}
	.rodata : { *(.rodata .rodata.* .constdata) }
	.data : { *(.data .data.*) }
	.bss : { *(.bss .bss.* COMMON) }
	/DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note* .ARM.attributes) }
}
ASSERT(sygl_entry_table == SYGL_BASE, "sygl: entry table must be at SYGL_BASE")
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "sygl: no writable static data allowed (the blob is ROM)")
