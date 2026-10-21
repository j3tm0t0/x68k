SECTIONS { . = 0xf4c; .text 0xf4c : ALIGN(2) { *(.text) } /DISCARD/ : { *(.data) *(.bss) } }
