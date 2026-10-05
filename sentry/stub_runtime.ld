ENTRY(stub_start)
SECTIONS {
  . = 0;
  .text : { *(.text.stub_start) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
  .got : { *(.got*) }
  ASSERT(SIZEOF(.got) == 0, "stub must not use absolute GOT entries")
  ASSERT(SIZEOF(.data) == 0, "stub must not contain initialized pointer tables")
  ASSERT(. < 65536, "stub code exceeds its reserved region")
  stub_boot = 65536;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
