/* gcebe blob: fixed ROM address, read-only (src/gcebe/gcebe.c). GCEBE_BASE comes from build.sh. */
ENTRY(gcebe_create)
SECTIONS
{
	. = GCEBE_BASE;
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
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "gcebe: no writable static data allowed (the blob is ROM)")
