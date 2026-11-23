/*
******************************************************************************
**
**  File        : LinkerScript.ld
**
**  Author      : STM32CubeIDE
**
**  Abstract    : Linker script for STM32U5A5xJ Device from STM32U5 series
**                      4096Kbytes FLASH
**                      2512Kbytes RAM
**
**                Set heap size, stack size and stack location according
**                to application requirements.
**
**                Set memory bank area and size if external memory is used.
**
**  Target      : STMicroelectronics STM32
**
**  Distribution: The file is distributed as is without any warranty
**                of any kind.
**
*****************************************************************************
** @attention
**
** Copyright (c) 2025 STMicroelectronics.
** All rights reserved.
**
** This software is licensed under terms that can be found in the LICENSE file
** in the root directory of this software component.
** If no LICENSE file comes with this software, it is provided AS-IS.
**
*****************************************************************************
*/

INCLUDE "libc_exports_kernel_0.0.3.ld"
INCLUDE "LibC/libc_exports_0.0.3.ld"

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM); /* end of "RAM" Ram type memory */

_Min_Heap_Size  = 0x200; /* required amount of heap */

/* MSP (main/interrupt) stack. Raised from 0x400 to 0x500 when MSPLIM was
   armed over this region: the guard needs a reserve below the limit
   for the overflow fault's own exception frame, and taking that out of a
   1 KB region would have left only 768 bytes usable. The extra 256 bytes
   keeps a full 1 KB usable with the reserve below it, so arming the guard
   costs no usable stack.

   It comes out of the heap, which is not free - see the RAM_CRASH note below
   for why every byte here is a byte the two-ended heap does not get.

   WORST MEASURED PEAK IS 664 BYTES, against the 1024 the guard allows - a
   margin of 360. That figure is the USB CDC path, not an idle watch: a quiet
   watch sits around 224 and an activity around 448, so quoting either of
   those would flatter the margin. If a change pushes the peak up, this
   number is the one to re-measure against. */
_Min_Stack_Size = 0x500; /* required amount of stack */

/* Memories definition */
MEMORY
{
  /* RAM_CRASH is sized to what it holds, not to a round number. The four
     NOLOAD sections placed in it below total 0x11808 (71,688 bytes):
     .crash_dump 6,112 + .log_buffer 65,552 + .systemlog 16 + .boot_flags 8.
     At 72K it has 2,040 bytes of slack; at the 128K it used to be, 59,392
     bytes were unreachable by anything.

     Every byte not reserved here is a byte of heap, because _estack is
     ORIGIN(RAM) + LENGTH(RAM) and the heap is the span between the end of
     .bss and _estack less the ISR stack - which is the limit _sbrk_r
     enforces ONCE THE SCHEDULER IS RUNNING (heap_useNewlib_ST.cpp:321-323).
     Before that it clamps to the live stack pointer instead, which is
     lower, so early allocations do not see this space. That does not change
     what the region is worth: everything that competes for the heap runs
     after the scheduler starts.

     It is worth more than its size once the two-ended heap lands:
     there the app image region is placed downward from that same
     expression, so both ends of the pool gain the full amount. That is a
     forward reference - no such code exists on this branch.

     IF ANY OF THE THREE SECTIONS GROWS, THIS MUST GROW WITH IT. The
     CrashDump capsule is the one that actually has: it is versioned and has
     gained fields. Overflowing the region is a link error, not a silent
     corruption - verified by test-linking at 69K, which fails with "region
     `RAM_CRASH' overflowed" - so the failure is safe, but it will look like
     an unrelated build break. */
  RAM        (xrw) : ORIGIN = 0x20000000, LENGTH = 2496K - 72K
  RAM_CRASH  (rw)  : ORIGIN = 0x20000000 + (2496K - 72K), LENGTH = 72K
/*  SRAM4 (xrw)      : ORIGIN = 0x28000000, LENGTH = 16K */ /* Used by LibC */
  FLASH (rx)       : ORIGIN = 0x08060000, LENGTH = 3712K /* 4096 - 384 */
}

/* Export memory bounds for crashdump code */
PROVIDE(__ram1_start__  = ORIGIN(RAM));
PROVIDE(__ram1_end__    = ORIGIN(RAM)  + LENGTH(RAM));
PROVIDE(__flash_start__ = ORIGIN(FLASH));
PROVIDE(__flash_end__   = ORIGIN(FLASH) + LENGTH(FLASH));

/* Sections */
SECTIONS
{
  /* The startup code into "FLASH" Rom type memory */
  .isr_vector :
  {
    KEEP(*(.isr_vector)) /* Startup code */
  } >FLASH

  /* The program code and other data into "FLASH" Rom type memory */
  .text :
  {
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data into "FLASH" Rom type memory */
  .rodata :
  {
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
  } >FLASH

  .ARM.extab (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } >FLASH
  .ARM (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* Used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections into "RAM" Ram type memory */
  .data :
  {
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */
    *(.RamFunc)        /* .RamFunc sections */
    *(.RamFunc*)       /* .RamFunc* sections */

    _edata = .;        /* define a global symbol at data end */
  } >RAM AT> FLASH

  /* Uninitialized data section into "RAM" Ram type memory */
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss section */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  /* Crash dump area at the end of RAM1, preserved across reset */
  .crash_dump (NOLOAD) :
  {
    . = ALIGN(8);
    KEEP(*(.crash_dump))
    . = ALIGN(8);
  } > RAM_CRASH

  /* System log ring buffer, preserved across reset (no-init) */
  .log_buffer (NOLOAD) :
  {
    . = ALIGN(8);
    KEEP(*(.log_buffer))
    . = ALIGN(8);
  } > RAM_CRASH

  /* System log drain cursor, preserved across reset (no-init) */
  .systemlog (NOLOAD) :
  {
    . = ALIGN(8);
    KEEP(*(.systemlog))
    . = ALIGN(8);
  } > RAM_CRASH

  /* Requests a software reset leaves for the next boot (no-init) */
  .boot_flags (NOLOAD) :
  {
    . = ALIGN(8);
    KEEP(*(.boot_flags))
    . = ALIGN(8);
  } > RAM_CRASH

  /* User_heap_stack section, used to check that there is enough "RAM" Ram type memory left */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* Remove information from the compiler libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

/* The MSP (main/interrupt) stack is the top _Min_Stack_Size bytes of RAM, and
   three things depend on that region's geometry: it is painted with
   a pattern at boot to measure the high-water mark, MSPLIM is armed over it,
   and the two-ended image allocator fills up to its bottom edge. These hold
   the assumptions that code makes and the linker can check.

   Word alignment: Heap_paintIsrStack fills the region a word at a time from
   _estack - _Min_Stack_Size. Misaligned, the last store would run past the
   end of the fill into the live MSP frame. */
ASSERT((_Min_Stack_Size & 7) == 0,
       "_Min_Stack_Size must be a multiple of 8 (MSPLIM ignores the low bits, and the paint is word-wise)")

/* Room for the MSPLIM fault reserve (ISR_STACK_FAULT_RESERVE, 256 bytes in
   heap_useNewlib_ST.cpp) plus something worth guarding. Below the reserve the
   arming code declines rather than placing the limit above _estack, so this
   assert is the earlier and louder of the two signals. */
ASSERT(_Min_Stack_Size >= 0x200,
       "_Min_Stack_Size must leave room for the MSPLIM fault reserve")
