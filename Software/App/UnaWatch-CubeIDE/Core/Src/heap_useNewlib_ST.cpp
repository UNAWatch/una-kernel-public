/**
 * \file heap_useNewlib_ST.cpp
 * \brief Thread-safe integration of newlib malloc with FreeRTOS using dual heaps.
 *
 * \par Overview
 * This file implements memory management redirection for newlib and FreeRTOS
 * to use separate heap regions:
 * - **FreeRTOS heap** is used exclusively for RTOS needs (tasks, queues, mutexes).
 * - **Newlib heap** (sbrk) is used for standard library functions (`printf`,
 * `strtok`, `mktime`) and general application allocations (`malloc`, `new`, etc.).
 *
 * **Reasons for choosing the dual-heap strategy:**
 * - **Avoiding fragmentation** since newlib and FreeRTOS use different allocation
 *     strategies.
 * - **Ensuring proper functionality of standard functions** (`printf`, `scanf`),
 *     which rely on `malloc`.
 * - **Better memory control for FreeRTOS** by keeping its heap smaller and more
 *     predictable, since most RTOS resources (tasks, queues) are statically allocated.
 * - **Providing thread safety for newlib** via `__malloc_lock/__malloc_unlock`.
 *
 * \par Modifications
 * All modifications are marked with `#if (USE_ORIGINAL == 0)`.
 * Key differences from the original implementation by Dave Nadler:
 * - **Updated heap boundary calculations for newlib:**
 *   - Uses `STM CubeMX`-generated linker symbols (`_Min_Stack_Size`, `_estack`)
 *     to avoid manual linker file modifications.
 * - **Updated newlib version checks:**
 *   - Code is verified for `newlib 4.3.0` and issues a warning if a different
 *     version is detected.
 * - **Added memory allocation logging:**
 *   - `LOG_DEBUG` messages added in `__wrap__malloc_r` and `__wrap__free_r` to
 *     track allocations.
 *   - Implemented `Heap_getFreeHeapSize()`, `Heap_getMinimumEverFreeHeapSize()`,
 *     and `Heap_getTotalHeapSize()` for heap usage monitoring.
 * - **Modified out-of-memory handling:**
 *   - Since FreeRTOS heap is no longer monitored, FreeRTOS memory failure handlers
 *     are not used.
 *   - Instead, `LOG_ERROR("Malloc failed!\n")` is logged when memory allocation fails.
 * - **Overloaded `operator new(std::nothrow)` to ensure proper behavior
 *     with `-fno-exceptions`**.
 * - **Handled `abort()` calls:**
 *   - `abort()` now triggers `configASSERT(0)`, causing an MCU reset on fatal errors.
 *
 * \author Dave Nadler (original implementation); modifications by UNA Watch Ltd.
 *         (https://www.unawatch.com)
 * \date August 20, 2019 (original); modifications from February 7, 2025
 *
 * \copyright
 * (c) Dave Nadler 2017-2020, All Rights Reserved.
 *
 *
 * **Original text:**
 *
 * \file heap_useNewlib_ST.c
 * \brief Wrappers required to use newlib malloc-family within FreeRTOS.
 *
 * \par Overview
 * Route FreeRTOS memory management functions to newlib's malloc family.
 * Thus newlib and FreeRTOS share memory-management routines and memory pool,
 * and all newlib's internal memory-management requirements are supported.
 *
 * \author Dave Nadler
 * \date 20-August-2019
 * \version  3-Jan-2023 Correct _malloc_r signature+call for malloc wrap
 * \version  3-Jan-2023 Function declarations and unused arguments for picky compiler
 * \version 27-Jun-2020 Correct "FreeRTOS.h" capitalization, commentary
 * \version 24-Jun-2020 commentary only
 * \version 11-Sep-2019 malloc accounting, comments, newlib version check
 *
 * \see http://www.nadler.com/embedded/newlibAndFreeRTOS.html
 * \see https://sourceware.org/newlib/libc.html#Reentrancy
 * \see https://sourceware.org/newlib/libc.html#malloc
 * \see https://sourceware.org/newlib/libc.html#index-_005f_005fenv_005flock
 * \see https://sourceware.org/newlib/libc.html#index-_005f_005fmalloc_005flock
 * \see https://sourceforge.net/p/freertos/feature-requests/72/
 * \see http://www.billgatliff.com/newlib.html
 * \see http://wiki.osdev.org/Porting_Newlib
 * \see http://www.embecosm.com/appnotes/ean9/ean9-howto-newlib-1.0.html
 *
 *
 * \copyright
 * (c) Dave Nadler 2017-2020, All Rights Reserved.
 * Web:         http://www.nadler.com
 * email:       drn@nadler.com
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * - Use or redistributions of source code must retain the above copyright notice,
 *   this list of conditions, and the following disclaimer.
 *
 * - Use or redistributions of source code must retain ALL ORIGINAL COMMENTS, AND
 *   ANY CHANGES MUST BE DOCUMENTED, INCLUDING:
 *   - Reason for change (purpose)
 *   - Functional change
 *   - Date and author contact
 *
 * - Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @brief Macro to mark all modifications
 */
#define USE_ORIGINAL 0

// ================================================================================================
// =======================================  Configuration  ========================================
// These configuration symbols could be provided by from build...
#define STM_VERSION // Replace sane LD symbols with STM CubeMX's poor standard exported LD symbols
#if (USE_ORIGINAL == 0)
#include <stddef.h>
extern size_t _Min_Stack_Size; // Symbol defined in the linker script
#define ISR_STACK_LENGTH_BYTES ((size_t)&_Min_Stack_Size) // bytes to reserve for ISR (MSP) stack
#else
#define ISR_STACK_LENGTH_BYTES (configISR_STACK_SIZE_WORDS*4) // bytes to reserve for ISR (MSP) stack
#endif // (USE_ORIGINAL == 0)
// =======================================  Configuration  ========================================
// ================================================================================================


#include <stdlib.h> // maps to newlib...
#include <malloc.h> // mallinfo...
#include <errno.h>  // ENOMEM
#include <stdbool.h>
#include <stddef.h>

#include "newlib.h"
#if (USE_ORIGINAL == 0)
// Pinned to newlib 4.4.0: what the CI/Docker toolchain (gnu-tools-for-stm32
// 13.3.rel1) ships, and therefore what every released binary is built with.
// A local CubeIDE 1.19 install carries gnu-tools 14.3.rel1 / newlib 4.5.0, so
// one `!=` cannot describe both and this guard deliberately names the CI one.
// A local build will warn. That is the intended signal, not noise: 4.5 is
// genuinely unverified here.
//
// Not a claim that the previous `!= 5` was written incorrectly -- the test has
// tracked the compiler while the warning string lagged it by one minor, twice.
// The pin stands on one thing only: 4.5 is not verified here. The malloc-family
// assumptions in this file were checked against 4.4.0, and nobody has repeated
// that work on 4.5.
//
// It deliberately does NOT stand on the page size, though that is the
// difference we happened to find. That value is chosen by SMALL_MEMORY, a flag
// used to build libc, from a source file identical in both versions -- so a 4.4
// libc built without it behaves like 4.5 while this guard stays silent. A
// version test cannot see a flag used to compile libc. The page size is
// therefore measured at runtime instead; see HeapStackMonitor::Start.
#if (__NEWLIB__ != 4) || (__NEWLIB_MINOR__ != 4)
  #warning "Built against newlib other than 4.4.0 (the CI toolchain). Expected for a local CubeIDE build, which ships 4.5.0; the malloc-family assumptions in this file are verified only for 4.4.0."
#endif
#else
#if ((__NEWLIB__ == 2) && (__NEWLIB_MINOR__ < 5)) || ((__NEWLIB__ == 4) && (__NEWLIB_MINOR__ > 2) || (__NEWLIB__ < 2) || (__NEWLIB__ > 4))
  #warning "This wrapper was verified for newlib versions 2.5 - 4.4; please ensure newlib's external requirements for malloc-family are unchanged!"
#endif
#endif // (USE_ORIGINAL == 0)

#include "FreeRTOS.h" // defines public interface we're implementing here
#if !defined(configUSE_NEWLIB_REENTRANT) ||  (configUSE_NEWLIB_REENTRANT!=1)
  #warning "#define configUSE_NEWLIB_REENTRANT 1 // Required for thread-safety of newlib sprintf, dtoa, strtok, etc..."
  // If you're *REALLY* sure you don't need FreeRTOS's newlib reentrancy support, comment out the above warning...
#endif
#include "task.h"

#if (USE_ORIGINAL == 0)
// Crash-safe sinks for the _sbrk_r out-of-memory breadcrumb; the regular
// logger cannot be used there (see the comment in _sbrk_r).
#include "LogBuffer/LogBuffer.hpp"
#include "LogUart.hpp"
#endif // (USE_ORIGINAL == 0)

#if (USE_ORIGINAL == 0)
extern "C" {
//#define MALLOCS_INSIDE_ISRs
// Added project logger to print information
#define LOG_MODULE_LEVEL    LOG_LEVEL_ERROR
#include "Logger.h"
// Added prototypes for statistic functions
size_t Heap_getFreeHeapSize( void );
size_t Heap_getMinimumEverFreeHeapSize( void );
size_t Heap_getTotalHeapSize( void );
size_t Heap_getSbrkHeadroom( void );
size_t Heap_getMinSbrkHeadroomEver( void );
size_t Heap_getTrimThreshold( void );
#endif // (USE_ORIGINAL == 0)

// ================================================================================================
// External routines required by newlib's malloc (sbrk/_sbrk, __malloc_lock/unlock)
// ================================================================================================

// Simplistic sbrk implementations assume stack grows downwards from top of memory,
// and heap grows upwards starting just after BSS.
// FreeRTOS normally allocates task stacks from a pool placed within BSS or DATA.
// Thus within a FreeRTOS task, stack pointer is always below end of BSS.
// When using this module, stacks are allocated from malloc pool, still always prior
// current unused heap area...

// Doesn't work with FreeRTOS: STM CubeMX 2018-2019 Incorrect Implementation
#if 0
    caddr_t _sbrk(int incr)
    {
        extern char end asm("end"); // From linker: lowest unused RAM address, just beyond end of BSS.
        static char *heap_end;
        char *prev_heap_end;
        if (heap_end == 0) heap_end = &end;
        prev_heap_end = heap_end;
        if (heap_end + incr > stack_ptr) // Fails here: always true for FreeRTOS task stacks
        {
            errno = ENOMEM; // ...so first call inside a FreeRTOS task lands here
            return (caddr_t) -1;
        }
        heap_end += incr;
        return (caddr_t) prev_heap_end;
    }
#endif

register char * stack_ptr asm("sp");

#ifdef STM_VERSION // Use STM CubeMX LD symbols for heap+stack area
    // To avoid modifying STM LD file (and then having CubeMX trash it), use available STM symbols
    // Unfortunately STM does not provide standardized markers for RAM suitable for heap!
    // STM CubeMX-generated LD files provide the following symbols:
    //    end     /* aligned first word beyond BSS */
    //    _estack /* one word beyond end of "RAM" Ram type memory, for STM32F429 0x20030000 */
    // Kludge below uses CubeMX-generated symbols instead of sane LD definitions
    #define __HeapBase  end
    #define __HeapLimit _estack // In K64F LD this is already adjusted for ISR stack space...
    static int heapBytesRemaining;
    // no DRN HEAP_SIZE symbol from LD... // that's (&__HeapLimit)-(&__HeapBase)
    uint32_t TotalHeapSize; // publish for diagnostic routines; filled in first _sbrk call.
    // Lowest headroom ever reached, i.e. the break's high-water mark. INT_MAX
    // until the first sbrk so an unstarted heap does not read as 0 headroom.
    static int sbrkMinHeadroomEver = 0x7FFFFFFF;
#else
    // Note: DRN's K64F LD provided: __StackTop (byte beyond end of memory), __StackLimit, HEAP_SIZE, STACK_SIZE
    // __HeapLimit was already adjusted to be below reserved stack area.
    extern char HEAP_SIZE;  // make sure to define this symbol in linker LD command file
    static int heapBytesRemaining = (int)&HEAP_SIZE; // that's (&__HeapLimit)-(&__HeapBase)
#endif


#ifdef MALLOCS_INSIDE_ISRs // STM code to avoid malloc within ISR (USB CDC stack)
    // We can't use vTaskSuspendAll() within an ISR.
    // STM's stunningly bad coding malpractice calls malloc within ISRs (for example, on USB connect function USBD_CDC_Init)
    // So, we must just suspend/resume interrupts, lengthening max interrupt response time, aarrggg...
    #warning "Add a check on ISR/Thread context and use the correct functions!"
    #define DRN_ENTER_CRITICAL_SECTION(_usis) { _usis = taskENTER_CRITICAL_FROM_ISR(); } // Disables interrupts (after saving prior state)
    #define DRN_EXIT_CRITICAL_SECTION(_usis)  { taskEXIT_CRITICAL_FROM_ISR(_usis);     } // Re-enables interrupts (unless already disabled prior taskENTER_CRITICAL)
#else
    #define DRN_ENTER_CRITICAL_SECTION(_usis) vTaskSuspendAll(); // Note: safe to use before FreeRTOS scheduler started, but not in ISR
    #define DRN_EXIT_CRITICAL_SECTION(_usis)  xTaskResumeAll();  // Note: safe to use before FreeRTOS scheduler started, but not in ISR
#endif

extern char __HeapBase, __HeapLimit;  // symbols from linker LD command file

#if (USE_ORIGINAL == 0)
// Append a plain string / the decimal digits of v into dst[n..cap); return the
// new n. Hand-rolled: the _sbrk_r OOM breadcrumb cannot use printf-family
// formatting (it may allocate, and the scheduler is suspended - see the
// comment on that path).
static uint32_t sbrkPutStr(char *dst, uint32_t cap, uint32_t n, const char *s) {
    while (*s != '\0' && n < cap) {
        dst[n++] = *s++;
    }
    return n;
}
static uint32_t sbrkPutDec(char *dst, uint32_t cap, uint32_t n, uint32_t v) {
    char tmp[10];
    uint32_t t = 0;
    do {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && t < sizeof(tmp));
    while (t > 0u && n < cap) {
        dst[n++] = tmp[--t];
    }
    return n;
}
static uint32_t sbrkPutInt(char *dst, uint32_t cap, uint32_t n, int v) {
    uint32_t mag;
    if (v < 0) {
        n = sbrkPutStr(dst, cap, n, "-");
        mag = (uint32_t)(-(v + 1)) + 1u; // negate without INT_MIN overflow
    } else {
        mag = (uint32_t)v;
    }
    return sbrkPutDec(dst, cap, n, mag);
}
#endif // (USE_ORIGINAL == 0)

// Use of vTaskSuspendAll() in _sbrk_r() is normally redundant, as newlib malloc family routines call
// __malloc_lock before calling _sbrk_r(). Note vTaskSuspendAll/xTaskResumeAll support nesting.

//! _sbrk_r version supporting reentrant newlib (depends upon above symbols defined by linker control file).
//! Where the kernel arena currently ends. Hoisted out of _sbrk_r so the
//! image side can ask whether a placement would collide with it; it was
//! already `static`, so lifetime and initialisation are unchanged.
static char *currentHeapEnd = &__HeapBase;

//! Floor published by the image allocator: the base of the lowest resident
//! image, which the break must never reach. NULL means no image region is
//! active and the kernel keeps the whole heap, exactly as before.
//!
//! Written only from inside a vTaskSuspendAll section, which is the same
//! exclusion _sbrk_r runs under (__malloc_lock and
//! DRN_ENTER_CRITICAL_SECTION are both vTaskSuspendAll, and they nest). So
//! the two cannot interleave and no new lock order is introduced. A plain
//! pointer is therefore sufficient - there is no lock-free protocol here to
//! get wrong.
static char *sImageFloor = NULL;

//! Bytes between the region top and the floor, i.e. everything the break can
//! no longer reach. Gaps BETWEEN images count: a gap above the floor is as
//! unreachable to the break as an image is, so this is the reserved span and
//! not the bytes actually in use.
static size_t sImageReserved = 0u;

extern "C" {

//! Top of the image region. The ISR stack is carved off the top of RAM and
//! must stay outside the region, so this is the same expression _sbrk_r uses
//! as its running limit - which also means an empty region leaves the kernel
//! exactly the heap it has today.
char *Heap_imageRegionTop(void) {
    return &__HeapLimit - ISR_STACK_LENGTH_BYTES;
}

//! Current end of the kernel arena. For the image allocator's collision
//! check; call it inside the same suspension as the placement it informs, or
//! the answer is stale the moment it is returned.
char *Heap_currentBreak(void) {
    return currentHeapEnd;
}

//! Publish the base of the lowest resident image, or NULL for none.
//! Call ONLY with the scheduler suspended - see sImageFloor.
void Heap_setImageFloor(char *floor) {
    sImageFloor = floor;

    // Keep the headroom accounting honest. heapBytesRemaining is documented as
    // the bytes sbrk can still hand out -- the fragmentation canary -- and
    // reserving the top of RAM takes bytes away from exactly that, even though
    // no sbrk call happened. Without this the watch would report free heap it
    // cannot actually give to the kernel, which is the one number heap
    // exhaustion is diagnosed from.
    const size_t reserved = (floor == NULL)
            ? 0u
            : (size_t)(Heap_imageRegionTop() - floor);

    heapBytesRemaining -= (int)reserved - (int)sImageReserved;
    sImageReserved      = reserved;

    // The break's high-water is really a headroom low-water, so a floor coming
    // down counts towards it just as the break going up does. Without this the
    // watermark would miss a peak caused entirely by images.
    if (heapBytesRemaining < sbrkMinHeadroomEver) {
        sbrkMinHeadroomEver = heapBytesRemaining;
    }
}

} // extern "C"

void * _sbrk_r(struct _reent *pReent, int incr) {
    (void)pReent;
    #ifdef MALLOCS_INSIDE_ISRs // block interrupts during free-storage use
      UBaseType_t usis; // saved interrupt status
    #endif
    #ifdef STM_VERSION // Use STM CubeMX LD symbols for heap
      if(TotalHeapSize==0) {
        TotalHeapSize = (int)((&__HeapLimit)-(&__HeapBase))-ISR_STACK_LENGTH_BYTES;
        // Less anything already reserved for images, so this lazy init cannot
        // silently undo a reservation. It cannot happen today (an image load
        // needs a filesystem, which has malloc'd long before), but the counter
        // should not depend on that ordering holding.
        heapBytesRemaining = (int)TotalHeapSize - (int)sImageReserved;
      }
    #endif
    char* limit = (xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED) ?
            stack_ptr   :  // Before scheduler is started, limit is stack pointer (risky!)
            &__HeapLimit-ISR_STACK_LENGTH_BYTES;  // Once running, OK to reuse all remaining RAM except ISR stack (MSP) stack
    // Two-ended heap: app images are placed downward from the top, so
    // the arena may not grow past the lowest one. Clamped rather than
    // replaced, so an unset or higher floor cannot accidentally RAISE the
    // limit, and so the pre-scheduler path is untouched - images cannot be
    // loaded before the scheduler runs.
    if ((sImageFloor != NULL) && (sImageFloor < limit)) {
        limit = sImageFloor;
    }
    DRN_ENTER_CRITICAL_SECTION(usis);
    if (currentHeapEnd + incr > limit) {
        // Ooops, no more memory available...
#if (USE_ORIGINAL == 0)
        // We are not monitoring the FreeRTOS heap, so report the exhaustion here.
        // NO LOG_* on this path: _sbrk_r runs with the scheduler suspended
        // (__malloc_lock and DRN_ENTER_CRITICAL_SECTION are both
        // vTaskSuspendAll), and the logger takes its mutex with a non-zero
        // timeout, which trips FreeRTOS's cannot-block-while-suspended
        // configASSERT before anything prints - the first heap exhaustion would
        // assert-reboot with no diagnostic and malloc could never return NULL to
        // its caller. Emit a breadcrumb through the crash-safe sinks instead:
        // the no-init log ring (bounded PRIMASK critical section, consumer
        // notify skipped) and the count-bounded HAL-free UART path. Rate-limited
        // so an allocate-until-NULL loop cannot flood the ring and evict the
        // pre-OOM history.
        static TickType_t lastOomLogTick;
        static bool oomLogged;
        const TickType_t nowTick = xTaskGetTickCount(); // critical-section read; safe while suspended
        if (!oomLogged || (TickType_t)(nowTick - lastOomLogTick) >= configTICK_RATE_HZ) {
            oomLogged = true;
            lastOomLogTick = nowTick;
            // The prefix (everything before the '(') is matched verbatim by
            // log analysis - keep it byte-stable. The suffix attributes
            // the failure: the increment sbrk could not serve and the headroom
            // left below __HeapLimit (headroom > 0 with a failed larger incr
            // means the heap died fragmented, not empty).
            char line[112];
            uint32_t n = sbrkPutStr(line, sizeof(line), 0,
                    "sbrk: newlib heap exhausted, malloc will return NULL (incr=");
            n = sbrkPutInt(line, sizeof(line), n, incr);
            n = sbrkPutStr(line, sizeof(line), n, ", headroom=");
            n = sbrkPutInt(line, sizeof(line), n, heapBytesRemaining);
            n = sbrkPutStr(line, sizeof(line), n, ")\n");
            LogBuffer::writeNoNotify(reinterpret_cast<const uint8_t*>(line),
                    static_cast<uint16_t>(n));
            Driver::LogUart::WriteRaw(line, n);
        }
        pReent->_errno = ENOMEM; // newlib's thread-specific errno
        DRN_EXIT_CRITICAL_SECTION(usis);
#else
        #if( configUSE_MALLOC_FAILED_HOOK == 1 )
          {
            extern void vApplicationMallocFailedHook( void );
            DRN_EXIT_CRITICAL_SECTION(usis);
            vApplicationMallocFailedHook();
          }
        #elif defined(configHARD_STOP_ON_MALLOC_FAILURE)
            // If you want to alert debugger or halt...
            // WARNING: brkpt instruction may prevent watchdog operation...
            while(1) { __asm("bkpt #0"); } // Stop in GUI as if at a breakpoint (if debugging, otherwise loop forever)
        #else
            // Default, if you prefer to believe your application will gracefully trap out-of-memory...
            pReent->_errno = ENOMEM; // newlib's thread-specific errno
            DRN_EXIT_CRITICAL_SECTION(usis);
        #endif
#endif // (USE_ORIGINAL == 0)
        return (char *)-1; // the malloc-family routine that called sbrk will return 0
    }
    // 'incr' of memory is available: update accounting and return it.
    char *previousHeapEnd = currentHeapEnd;
    currentHeapEnd += incr;
    heapBytesRemaining -= incr;
    // High-water of the break itself. mallinfo's usmblks is meant to carry
    // this but newlib never maintains it (it reads 0 in every sample), and a
    // watermark sampled from a 3 s timer misses peaks that open and close in
    // between. Tracked here instead, where it is exact: this is the only
    // place the break ever moves, in either direction (malloc_trim passes a
    // negative incr).
    if (heapBytesRemaining < sbrkMinHeadroomEver) {
        sbrkMinHeadroomEver = heapBytesRemaining;
    }
    DRN_EXIT_CRITICAL_SECTION(usis);
    return (char *) previousHeapEnd;
}
//! non-reentrant sbrk uses is actually reentrant by using current context
// ... because the current _reent structure is pointed to by global _impure_ptr
#if (USE_ORIGINAL == 0)
// Just type conversion
char * sbrk(int incr) { return static_cast<char*>(_sbrk_r(_impure_ptr, incr)); }
#else
char * sbrk(int incr) { return _sbrk_r(_impure_ptr, incr); }
#endif // (USE_ORIGINAL == 0)
//! _sbrk is a synonym for sbrk.
char * _sbrk(int incr) { return sbrk(incr); }

#ifdef MALLOCS_INSIDE_ISRs // block interrupts during free-storage use
  static UBaseType_t malLock_uxSavedInterruptStatus;
#endif
void __malloc_lock(struct _reent *r)   {
  (void)(r);
  #if defined(MALLOCS_INSIDE_ISRs)
    DRN_ENTER_CRITICAL_SECTION(malLock_uxSavedInterruptStatus);
  #else
    bool insideAnISR = xPortIsInsideInterrupt();
    configASSERT( !insideAnISR ); // Make damn sure no more mallocs inside ISRs!!
  vTaskSuspendAll();
  #endif
}
void __malloc_unlock(struct _reent *r) {
  (void)(r);
  #if defined(MALLOCS_INSIDE_ISRs)
    DRN_EXIT_CRITICAL_SECTION(malLock_uxSavedInterruptStatus);
  #else
  (void)xTaskResumeAll();
  #endif
}

// newlib also requires implementing locks for the application's environment memory space,
// accessed by newlib's setenv() and getenv() functions.
// As these are trivial functions, momentarily suspend task switching (rather than semaphore).
// Not required (and trimmed by linker) in applications not using environment variables.
// ToDo: Move __env_lock/unlock to a separate newlib helper file.
void __env_lock(void)    {       vTaskSuspendAll(); }
void __env_unlock(void)  { (void)xTaskResumeAll();  }

#if 1 // Provide malloc debug and accounting wrappers
  /// /brief  Wrap malloc/malloc_r to help debug who requests memory and why.
  /// To use these, add linker options: -Xlinker --wrap=malloc -Xlinker --wrap=_malloc_r
  // Note: These functions are normally unused and stripped by linker.
  size_t TotalMallocdBytes;
  int MallocCallCnt;
  static bool inside_malloc;
  void *__wrap_malloc(size_t nbytes) {
    extern void * __real_malloc(size_t nbytes);
    MallocCallCnt++;
    TotalMallocdBytes += nbytes;
    inside_malloc = true;
      void *p = __real_malloc(nbytes); // will call malloc_r...
    inside_malloc = false;
    return p;
  }
  void *__wrap__malloc_r(void *reent, size_t nbytes) {
    extern void * __real__malloc_r(void *reent,size_t nbytes);
    if(!inside_malloc) {
      MallocCallCnt++;
      TotalMallocdBytes += nbytes;
    }
    void *p = __real__malloc_r(reent,nbytes);
#if (USE_ORIGINAL == 0)
    LOG_DEBUG("[MALLOC] 0x%08X  %-8u Available: %-8u Minimum: %-8u\n", p, nbytes,
            Heap_getFreeHeapSize(), Heap_getMinimumEverFreeHeapSize());
#endif // (USE_ORIGINAL == 0)
    return p;
  }
#if (USE_ORIGINAL == 0)
  void __wrap__free_r(struct _reent *reent, void *ptr) {
    extern void __real__free_r(struct _reent *reent, void *ptr);
    __real__free_r(reent, ptr);
    if (ptr) {  // ptr can be NULL. It is expected.
      LOG_DEBUG("  [FREE  ] 0x%08X           Available: %-8u Minimum: %-8u\n", ptr,
             Heap_getFreeHeapSize(), Heap_getMinimumEverFreeHeapSize());
    }
  }
#endif // (USE_ORIGINAL == 0)
#endif

#if (USE_ORIGINAL == 0)

  // Trim threshold. dlmalloc's default is 128 KiB (DEFAULT_TRIM_THRESHOLD in
  // newlib's _mallocr.c, confirmed at 0x00020000 in this image's .data), and
  // free() only returns the top chunk to the break once it exceeds this.
  //
  // That default loses large allocations outright. malloc_extend_top asks sbrk
  // for `nb + top_pad + MINSIZE` and never credits the top chunk it already
  // holds, so a top below the threshold is memory the allocator can neither
  // reuse for a big request nor hand back. A measured failure had a clean
  // 98,424-byte top and 404,352 of headroom -- 502,776 contiguous against a
  // 454,272 request -- and failed anyway. Trimming first would have made the
  // headroom 502,776 and the request would have succeeded.
  //
  // Choosing the value: it must sit BELOW the largest recurring transient, not
  // below the carried-forward top. After a trim the top is ~100 bytes, so the
  // next transient of size S leaves a top of about S+200 when freed -- and that
  // is un-trimmable if S+200 is under the threshold.
  //
  // That ~100 figure is newlib 4.4, which is what CI builds. _malloc_trim_r
  // computes
  //     extra = (((top_size - 17 - pad + pagesz) / pagesz) - 1) * pagesz
  // and bails when pagesz > extra, with pagesz from sysconf(_SC_PAGESIZE).
  // So the smallest top that can be trimmed at all is pagesz + 17, and the
  // residual afterwards is under one page:
  //
  //     newlib 4.4  pagesz  128   min trimmable top    145   residual ~128
  //     newlib 4.5  pagesz 4096   min trimmable top  4,113   residual ~4,096
  //
  // 8 KiB clears both, but the margin is 57x on 4.4 and barely 2x on 4.5, and
  // anything below ~4 KiB would be silently dead on 4.5 -- the top would simply
  // never be returned. Do not lower this without re-checking against the
  // toolchain in use. The dominant transient
  // here is the AGNSS/EPO buffer, a single 64,512-byte reserve
  // (Aiding::scanEpoTimeRangeLocked), which is why 64 KiB parked the top at
  // ~64.7 KB for whole activities: 808 bytes above the threshold, never
  // returned. Picking 64 KiB against the ~98 KB carried top was measuring the
  // wrong thing, since trimming removes that top anyway.
  //
  // 8 KiB is below every recurring transient observed. sbrk here is pointer
  // arithmetic, so trimming often costs essentially nothing. Set to 0 to
  // restore stock behaviour for an A/B.
  #define HEAP_TRIM_THRESHOLD_BYTES (8u * 1024u)

  void __attribute__((constructor)) initHeap() {
      TotalHeapSize = heapBytesRemaining = (int)((&__HeapLimit)-(&__HeapBase))-ISR_STACK_LENGTH_BYTES;

#if (HEAP_TRIM_THRESHOLD_BYTES > 0)
      // newlib's mallopt for M_TRIM_THRESHOLD does nothing but store the
      // value, so THIS call has no ordering requirement against the first
      // allocation; it applies from the next free() onwards. Said of mallopt
      // only -- the TotalHeapSize/heapBytesRemaining assignment above it is a
      // different matter, since a constructor that allocated before this ran
      // would already have moved the break.
      (void)mallopt(M_TRIM_THRESHOLD, (int)HEAP_TRIM_THRESHOLD_BYTES);
#endif
  }

  // The changes I made are only for newlib thread safety and memory allocation tracking,
  // so let's just add functions to get statistics.
  size_t Heap_getFreeHeapSize( void ) PRIVILEGED_FUNCTION {
      struct mallinfo mi = mallinfo(); // available space now managed by newlib
      return mi.fordblks + heapBytesRemaining; // plus space not yet handed to newlib by sbrk
  }

  size_t Heap_getMinimumEverFreeHeapSize( void ) PRIVILEGED_FUNCTION {
#ifndef NDEBUG
      // Was TotalHeapSize - totalBytesProvidedBySBRK, which is not a minimum:
      // that counter drops on every trim (negative incr), so the figure ROSE
      // each time memory was handed back. Trimming is now frequent enough for
      // that to be actively misleading, and the break high-water tracked in
      // _sbrk_r is the quantity this was always reaching for.
      return Heap_getMinSbrkHeadroomEver();
#else
      return TotalHeapSize; // not tracking, so return maximum value
#endif
  }

  size_t Heap_getTotalHeapSize( void ) PRIVILEGED_FUNCTION {
      return TotalHeapSize;
  }

  // ==============================================================================================
  // MSP (ISR) stack watermark.
  //
  // The main stack occupies the top ISR_STACK_LENGTH_BYTES of RAM: it grows DOWN
  // from _estack (== __HeapLimit), and _sbrk_r keeps the heap below it. MSPLIM
  // is armed over it (Heap_armIsrStackLimit), so an overflow faults rather than
  // corrupting what sits below - these measure how close it comes to that.
  //
  // It matters more since the two-ended heap: the highest app image is placed
  // flush against this region and its last section is that app's task stack, so
  // an MSP overflow now lands in a running app's frames rather than in heap the
  // break rarely reached.
  // ==============================================================================================

  #define ISR_STACK_PAINT 0xC5C5C5C5u

  //! Set once Heap_paintIsrStack has run, so the readers below can tell "not
  //! measured yet" from "measured, and it is full".
  //!
  //! The monitor timer is started in main() BEFORE StartupTask is created, so
  //! in principle a check could land before the paint. Today it cannot - the
  //! first check is 3 s in and the paint happens within microseconds of the
  //! first task running - but the failure if that ever inverted is silent and
  //! permanent: the scan would find no pattern, report the full region, latch
  //! the peak at its maximum and never fire the residue dump again for the
  //! rest of the boot. Cheaper to make it impossible than to rely on 3 s.
  static volatile bool sIsrStackPainted = false;

  //! Bottom of the MSP stack region, which is also the top of the image area.
  //!
  //! Deliberately delegates rather than recomputing `&__HeapLimit -
  //! ISR_STACK_LENGTH_BYTES`: this is the floor the paint and MSPLIM write
  //! down to, and Heap_imageRegionTop() is the ceiling _sbrk_r and the
  //! two-ended image allocator fill up to. The two must be the same address.
  //! If they ever diverged, either the guard would sit inside live heap - so
  //! the paint clobbers an app image at boot and MSPLIM stops firing where it
  //! should - or the heap grows into the painted region.
  static char *IsrStackBottom( void ) {
      return Heap_imageRegionTop();
  }

  //! Fill the unused part of the MSP stack with a known pattern.
  //!
  //! CALL THIS FROM A TASK, NOT FROM main(). Before the scheduler starts,
  //! main() itself runs on MSP from _estack, so painting then either clobbers
  //! its live frames or is overwritten by the rest of init - and in any case
  //! vPortStartFirstTask reloads MSP from VTOR[0] on the way to the first task
  //! (portasm.c), so a pre-scheduler measurement would report boot depth rather
  //! than ISR depth. From a task the CPU is in thread mode on PSP and the whole
  //! region is idle.
  //!
  //! Only the region BELOW the live MSP is painted. On a full-descending stack
  //! everything at or above SP is in use, so this is safe whenever it runs.
  //! Interrupts are masked meanwhile: an interrupt taken mid-paint would stack
  //! its frame into the very words being written.
  void Heap_paintIsrStack( void ) PRIVILEGED_FUNCTION {
      const uint32_t primask = __get_PRIMASK();
      __disable_irq();

      uint32_t *p   = (uint32_t *)IsrStackBottom();
      uint32_t *end = (uint32_t *)(__get_MSP() & ~3u);
      while (p < end) {
          *p++ = ISR_STACK_PAINT;
      }

      sIsrStackPainted = true;
      __set_PRIMASK(primask);
  }

  //! Bytes kept below MSPLIM so a stack-overflow fault can be taken at all.
  //!
  //! This covers ONE thing: the exception entry frame, at most 104 bytes with
  //! FP state. The hardware is allowed to stack that below the limit - which
  //! is the whole reason the reserve exists - and the handler then clears
  //! MSPLIM in its first two instructions (stm32u5xx_it.cpp), so everything
  //! after that point is unguarded and does not need covering here.
  //!
  //! That the crash-dump chain then runs off the bottom of the region and
  //! into the image area below is deliberate. The alternative - leaving the
  //! limit in place, or lowering it only to the region bottom - means the
  //! dump re-violates it and the core locks up, which was measured (`crash
  //! msp` before the handler fix: watchdog reset, capsule with every register
  //! zero). A dump that scribbles a few hundred bytes into an app image
  //! microseconds before a reset is worth far more than a guard that costs us
  //! the dump.
  #define ISR_STACK_FAULT_RESERVE 256u

  //! Arm the hardware guard on the MSP stack.
  //!
  //! Until now nothing guarded this stack at all - tasks get PSPLIM from the
  //! FreeRTOS port, MSP got nothing - so an overflow silently corrupted
  //! whatever sat below it, which since the two-ended heap is a running app's
  //! image. This makes it a deterministic fault with a backtrace instead.
  //!
  //! Call this from the first task, for the same reason the painting is done
  //! there: main() runs on MSP from _estack before the scheduler and may go
  //! deeper than the reservation, so arming earlier could fault on a boot
  //! path that was never the thing being guarded. Once vPortStartFirstTask
  //! has reloaded MSP from VTOR[0], only handlers use it, and that is exactly
  //! the regime this is meant to bound.
  //! Refuses to arm if the region cannot hold the reserve. ISR_STACK_LENGTH_BYTES
  //! is a LINKER ADDRESS (&_Min_Stack_Size), not a constant, so static_assert
  //! cannot see it - and getting this wrong is not a subtle failure: the limit
  //! would land above _estack and the first interrupt after this point would
  //! fault forever. Leaving the stack unguarded is the safe direction.
  void Heap_armIsrStackLimit( void ) PRIVILEGED_FUNCTION {
      if (ISR_STACK_LENGTH_BYTES <= ISR_STACK_FAULT_RESERVE) {
          return;
      }
      __set_MSPLIM((uint32_t)(IsrStackBottom() + ISR_STACK_FAULT_RESERVE));
  }

  //! Bytes of MSP usable before the limit trips (the region less the reserve).
  //! Returns the whole region when the guard declined to arm, so the reported
  //! figure always matches what is actually enforced rather than underflowing.
  size_t Heap_isrStackUsableSize( void ) PRIVILEGED_FUNCTION {
      if (ISR_STACK_LENGTH_BYTES <= ISR_STACK_FAULT_RESERVE) {
          return ISR_STACK_LENGTH_BYTES;
      }
      return ISR_STACK_LENGTH_BYTES - ISR_STACK_FAULT_RESERVE;
  }

  //! Deepest the MSP has ever reached, in bytes used.
  //!
  //! Scans up from the bottom for the first word that is no longer the pattern;
  //! everything from there to _estack has been touched. Reads only, so it is
  //! safe from any context. Returns the full region size if the paint never ran
  //! or the stack has been driven to the bottom - those are indistinguishable
  //! from here, which is itself the signal that the reservation is too small.
  size_t Heap_isrStackHighWater( void ) PRIVILEGED_FUNCTION {
      const uint32_t *const bottom = (const uint32_t *)IsrStackBottom();
      const uint32_t *const top    = (const uint32_t *)&__HeapLimit;
      const uint32_t *p = bottom;

      if (!sIsrStackPainted) {
          return 0u;          // unmeasured, not full - see sIsrStackPainted
      }

      while (p < top && *p == ISR_STACK_PAINT) {
          p++;
      }
      return (size_t)((const char *)top - (const char *)p);
  }

  //! Bytes reserved for the MSP stack (_Min_Stack_Size).
  size_t Heap_isrStackSize( void ) PRIVILEGED_FUNCTION {
      return ISR_STACK_LENGTH_BYTES;
  }

  //! Copy what the deepest MSP excursion left behind, lowest address first.
  //!
  //! The high-water number says how deep the stack went; it does not say what
  //! went there, and a static call-graph analysis disagrees with the
  //! measurement by more than a hundred bytes. This recovers the evidence.
  //!
  //! Words below the live MSP keep whatever the last excursion to that depth
  //! wrote, and nothing shallower can overwrite them - a shallower interrupt
  //! stacks at HIGHER addresses. So the words just above the low-water mark
  //! are frame residue from the deepest nesting that ever happened, and stay
  //! valid until something deeper occurs. Reliability falls off towards the
  //! top, where later shallow interrupts churn constantly, so a reader should
  //! trust the low end and treat the far end as stale.
  //!
  //! Read-only and takes no lock. Note it scans the WHOLE region up to
  //! _estack, including whatever is live on MSP at the time - it does not
  //! stop below the current stack pointer. That is safe because it only ever
  //! reads, and because the caller is a task, where MSP is idle near the top.
  //! A concurrent interrupt can land in the window and rewrite part of it,
  //! which costs accuracy in the dump, never safety.
  //!
  //! Returns words copied; \p startAddr receives the address of the first,
  //! so the caller can label the offsets with somewhere real.
  size_t Heap_isrStackResidue( uint32_t *out, size_t maxWords,
                               uint32_t *startAddr ) PRIVILEGED_FUNCTION {
      const uint32_t *const bottom = (const uint32_t *)IsrStackBottom();
      const uint32_t *const top    = (const uint32_t *)&__HeapLimit;
      const uint32_t *p = bottom;
      size_t n = 0;

      if ((out == NULL) || (maxWords == 0u) || !sIsrStackPainted) {
          return 0u;
      }

      while ((p < top) && (*p == ISR_STACK_PAINT)) {
          p++;
      }
      if (startAddr != NULL) {
          *startAddr = (uint32_t)p;
      }

      while ((p < top) && (n < maxWords)) {
          out[n++] = *p++;
      }
      return n;
  }

  // Bytes of the span reserved for app images, i.e. the part of the total the
  // break can no longer reach. Reported so the three quantities add up in a
  // log line: sbrk headroom + this + what sbrk has handed out = the total.
  // A word read needs no exclusion; the value is only ever written whole.
  size_t Heap_getImageReserved( void ) PRIVILEGED_FUNCTION {
      return sImageReserved;
  }

  // Bytes sbrk has never handed to newlib. The fragmentation canary: once
  // it reaches 0, dlmalloc can only serve requests from free()d holes
  // (mallinfo fordblks) and a large allocation may fail with plenty of total
  // free space.
  size_t Heap_getSbrkHeadroom( void ) PRIVILEGED_FUNCTION {
      return (heapBytesRemaining > 0) ? (size_t)heapBytesRemaining : 0u;
  }

  // Lowest headroom the break has ever reached. Exact rather than sampled --
  // updated inside _sbrk_r, the only place the break moves. Subtract from the
  // total to get peak sbrk'd bytes. Replaces mallinfo's usmblks, which newlib
  // does not maintain and which reads 0 in every sample.
  // The configured trim threshold, so the page-size check in HeapStackMonitor
  // can compare against it. _malloc_trim_r refuses to return anything when the
  // top chunk is smaller than pagesz + 17, so a threshold below that is
  // silently inert -- and the page size is a runtime property of whichever
  // libc linked, not something any preprocessor test here can see.
  size_t Heap_getTrimThreshold( void ) PRIVILEGED_FUNCTION {
#if (HEAP_TRIM_THRESHOLD_BYTES > 0)
      return (size_t)HEAP_TRIM_THRESHOLD_BYTES;
#else
      // The override is off, so mallopt is never called and dlmalloc's own
      // DEFAULT_TRIM_THRESHOLD is what is in force. Reporting 0 here would
      // describe a setting that was never applied.
      return (size_t)(128u * 1024u);
#endif
  }

  size_t Heap_getMinSbrkHeadroomEver( void ) PRIVILEGED_FUNCTION {
      if (sbrkMinHeadroomEver == 0x7FFFFFFF) {
          return Heap_getSbrkHeadroom();  // no sbrk yet
      }
      return (sbrkMinHeadroomEver > 0) ? (size_t)sbrkMinHeadroomEver : 0u;
  }

  // Abort can be called from some newlib functions that do not expect to receive
  // a memory allocation error.
  void abort() {
      LOG_ERROR("Abort called!\n");
      configASSERT(0);  // This will reset MCU
  }
#else
// ================================================================================================
// Implement FreeRTOS's memory API using newlib-provided malloc family.
// ================================================================================================

void *pvPortMalloc( size_t xSize ) PRIVILEGED_FUNCTION {
    void *p = malloc(xSize);
    return p;
}
void vPortFree( void *pv ) PRIVILEGED_FUNCTION {
    free(pv);
}

size_t xPortGetFreeHeapSize( void ) PRIVILEGED_FUNCTION {
    struct mallinfo mi = mallinfo(); // available space now managed by newlib
    return mi.fordblks + heapBytesRemaining; // plus space not yet handed to newlib by sbrk
}

// GetMinimumEverFree is not available in newlib's malloc implementation.
// So, no implementation is provided: size_t xPortGetMinimumEverFreeHeapSize( void ) PRIVILEGED_FUNCTION;

//! No implementation needed, but stub provided in case application already calls vPortInitialiseBlocks
void vPortInitialiseBlocks( void ) PRIVILEGED_FUNCTION {}

#endif // (USE_ORIGINAL == 0)

#if (USE_ORIGINAL == 0)
} // extern "C"


//    The `operator new` should throw an exception in case of memory
//    allocation failure. Since there are no exceptions in the project
//    (using `-fno-exceptions`), the `operator new` calls abort().
//    It seems that this is the expected behavior.
//
//    An overloaded version of the `operator new` that does not throw
//    exceptions can be interpreted by the compiler as one that does
//    not return nullptr under any circumstances. If we even try to check
//    the result for nullptr, the compiler may skip the check during
//    optimization.
//
//    If we need the `operator new` to return nullptr so that we can check
//    whether the memory allocation was successful, we usually use the
//    `operator new(std::nothrow)`. However, even in this case, newlib
//    version 4.3.0 with `-fno-exceptions` and `--specs=nosys.specs`
//    calls abort(). Perhaps this is because the `operator new(std::nothrow)`
//    calls the regular `operator new`.
//      (https://github.com/picolibc/picolibc/discussions/685)
//
//    So, the best solution I came up with is just an overload of the `operator new`.

#include <new>

_GLIBCXX_NODISCARD void* operator new(std::size_t sz, const std::nothrow_t&) _GLIBCXX_USE_NOEXCEPT
{
    return malloc(sz);
}

_GLIBCXX_NODISCARD void* operator new[](std::size_t sz, const std::nothrow_t&) _GLIBCXX_USE_NOEXCEPT
{
    return malloc(sz);
}

void operator delete(void* ptr, const std::nothrow_t&) _GLIBCXX_USE_NOEXCEPT
{
    free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) _GLIBCXX_USE_NOEXCEPT
{
    free(ptr);
}

//    std::terminate() defaults to libstdc++'s verbose handler, which demangles
//    the exception's type name and prints it, with what(), to stderr before
//    calling abort(). Here stderr goes nowhere (_write() in syscalls.c
//    discards it), so all that handler adds is the demangler: ~33 KB of flash.
//    Defining the handler drops it; terminate still ends in the abort() above,
//    which logs and asserts into the crash dump as before.
//    (std::set_terminate(std::abort) would not help: libstdc++ still
//    references the default handler, and that keeps the demangler linked.)
namespace __gnu_cxx {
void __verbose_terminate_handler()
{
    abort();
}
} // namespace __gnu_cxx

#endif // (USE_ORIGINAL == 0)
