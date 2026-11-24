/**
 ******************************************************************************
 * @file    HeapStackMonitor.hpp
 * @brief
 ******************************************************************************
 *
 ******************************************************************************
 */

#define LOG_MODULE_PRX      "HeapStackMonitor"
#define LOG_MODULE_LEVEL    LOG_LEVEL_DEBUG
#include "Logger.h"

#include "HeapStackMonitor.hpp"
#include "Shared/HeapTelemetry.hpp"

#include <malloc.h>
#include <unistd.h>   // sysconf(_SC_PAGESIZE): the allocator's runtime page size

extern "C" size_t Heap_getFreeHeapSize();
extern "C" size_t Heap_getMinimumEverFreeHeapSize();
extern "C" size_t Heap_getTotalHeapSize();
extern "C" size_t Heap_getSbrkHeadroom();
extern "C" size_t Heap_getMinSbrkHeadroomEver();
extern "C" size_t Heap_getImageReserved();

// MSP (ISR) stack watermark. MSPLIM bounds the main stack, but a limit only
// says where the cliff is; these say how close the watch runs to it.
extern "C" size_t Heap_isrStackHighWater();
extern "C" size_t Heap_isrStackSize();
extern "C" size_t Heap_isrStackResidue(uint32_t *out, size_t maxWords,
                                       uint32_t *startAddr);
extern "C" size_t Heap_isrStackUsableSize();
extern "C" size_t Heap_getTrimThreshold();

// malloc accounting kept by the __wrap__malloc_r wrappers (heap_useNewlib).
extern "C" size_t TotalMallocdBytes;
extern "C" int MallocCallCnt;

namespace {

/// Words of residue to copy and scan. A narrower window misses the caller:
/// the deepest few hundred bytes are the formatter itself, and whoever
/// called it sits above that. Not widened to the full 1.25 KB region - the
/// top is the churned end that a reader should not trust, and
/// Heap_isrStackResidue caps itself at the measured peak regardless.
constexpr size_t skIsrResidueWords = 256;

/// Words per dump line, sized for the number of log WRITES rather than for
/// readability: a burst long enough to outrun the logger gets another task's
/// output interleaved into it and loses the rest, which costs exactly the
/// deep sample the dump exists to capture. 16 covers the window in 16
/// writes, each ~156 characters of payload plus ~75 of prefix, inside
/// LOG_BUFFER_SIZE (512, thread context).
///
/// Do not switch to LOG_INFO_DUMP: Logger_hexdump_safe writes once per 16
/// BYTES (Logger.c:501), which is 64 writes for the same window.
constexpr size_t skWordsPerLine = 16;

} // namespace

// Static fields
size_t HeapStackMonitor::mOsHeapTreshold = 5 * 1024;
size_t HeapStackMonitor::mSysHeapTreshold = 5 * 1024;
size_t HeapStackMonitor::mStackTreshold = 100;
TaskStatus_t HeapStackMonitor::mTaskInfoArray[skTasksMax];
TimerHandle_t HeapStackMonitor::mTimHandle;
StaticTimer_t HeapStackMonitor::mTimControlBlock;
uint32_t HeapStackMonitor::mCheckCount;
size_t HeapStackMonitor::mSysHeapMinEver = SIZE_MAX;
volatile CrashHeapSample HeapStackMonitor::mHeapSample;

// Change-gate snapshot for the periodic attribution line (see Check()).
bool HeapStackMonitor::mLastInfoValid = false;
size_t HeapStackMonitor::mLastInfoFreeHeap = 0;
size_t HeapStackMonitor::mLastInfoMinEver = SIZE_MAX;
uint32_t HeapStackMonitor::mLastInfoNotifCount = 0;
uint32_t HeapStackMonitor::mLastInfoScanCount = 0;
uint32_t HeapStackMonitor::mLastInfoAppBytes = 0;
uint32_t HeapStackMonitor::mLastInfoAppBlocks = 0;
size_t HeapStackMonitor::mIsrStackPeak = 0;

void HeapStackMonitor::DumpIsrStackResidue(size_t used)
{
    // Static, not automatic: this is stack-investigation code, and taking a kilobyte of
    // task stack to investigate a stack problem would be poor form. Only
    // this function touches it, and only from the monitor timer task.
    static uint32_t words[skIsrResidueWords];
    uint32_t        base = 0;

    // Copy first, print second. Logging is not instant and an interrupt
    // landing mid-dump would rewrite the very words being reported; taking
    // the snapshot in one pass means the printed lines are all from the
    // same moment even if the region moves underneath afterwards.
    const size_t got = Heap_isrStackResidue(words, skIsrResidueWords, &base);
    if (got == 0u) {
        return;
    }

    // "limit" is where MSPLIM sits, which is the number that actually bites:
    // the region less the reserve the overflow fault needs for its own entry.
    LOG_INFO("MSP peak %u/%u bytes (limit %u), residue from 0x%08x:\n",
             (unsigned)used, (unsigned)Heap_isrStackSize(),
             (unsigned)Heap_isrStackUsableSize(), (unsigned)base);

    // Raw hex, no interpretation. Deciding which words are return addresses
    // needs the ELF's section bounds, and the watch has no symbol table: the
    // first version tested "odd and somewhere in flash", which admits every
    // rodata and font word too, and the only survivor of the deepest dump
    // turned out to be one of those false positives. Decoding offline
    // against the ELF has the real .text bounds, so leave it to that and
    // keep the watch dumb.
    //
    // A partial trailing line is dropped rather than special-cased: at most
    // the last 15 words of the window, which is the churned end of the
    // residue that a reader should not trust anyway.
    const size_t lines = got / skWordsPerLine;
    for (size_t i = 0; i < lines; i++) {
        const uint32_t *const w = &words[i * skWordsPerLine];

        LOG_INFO("  MSP +%03u %08x %08x %08x %08x %08x %08x %08x %08x "
                 "%08x %08x %08x %08x %08x %08x %08x %08x\n",
                 (unsigned)(i * skWordsPerLine * sizeof(uint32_t)),
                 (unsigned)w[0],  (unsigned)w[1],  (unsigned)w[2],
                 (unsigned)w[3],  (unsigned)w[4],  (unsigned)w[5],
                 (unsigned)w[6],  (unsigned)w[7],  (unsigned)w[8],
                 (unsigned)w[9],  (unsigned)w[10], (unsigned)w[11],
                 (unsigned)w[12], (unsigned)w[13], (unsigned)w[14],
                 (unsigned)w[15]);
    }
}

size_t HeapStackMonitor::GetSysHeapSize()
{
    return Heap_getTotalHeapSize();
}

size_t HeapStackMonitor::GetSysHeapFree()
{
    return Heap_getFreeHeapSize();
}

size_t HeapStackMonitor::GetSysHeapWaterMark()
{
    return Heap_getMinimumEverFreeHeapSize();
}

size_t HeapStackMonitor::GetOsHeapSize()
{
    return configTOTAL_HEAP_SIZE;
}

size_t HeapStackMonitor::GetOsHeapFree()
{
    return xPortGetFreeHeapSize();
}

size_t HeapStackMonitor::GetOsHeapWaterMark()
{
    return xPortGetMinimumEverFreeHeapSize();
}

size_t HeapStackMonitor::GetTasksInfo(TaskStatus_t *pInfo, size_t size)
{
    assert(pInfo != nullptr);
    size_t pulTotalRunTime = 0;
    return uxTaskGetSystemState(pInfo, size, &pulTotalRunTime);
}

void HeapStackMonitor::Start(uint32_t checkPeriod)
{
    // Report the allocator's page size once, and complain if the trim
    // threshold is below what the allocator can act on. Both are runtime
    // properties of whichever libc was linked: newlib reads the page size from
    // sysconf, and the value is chosen by SMALL_MEMORY, a flag used to build
    // libc that no preprocessor test here can see.
    //
    // Below pagesz + 17 the threshold is INERT rather than fatal: free() still
    // calls systrim once the top reaches the threshold, and systrim declines
    // until the top reaches pagesz + 17, then trims. So the top is still
    // returned, just at a size the threshold did not choose -- which makes the
    // number misleading rather than broken, and invisible without this line.
    const long   pagesz = sysconf(_SC_PAGESIZE);
    const size_t trim   = Heap_getTrimThreshold();

    if (pagesz <= 0) {
        LOG_WARNING("malloc page size unavailable\n");
    } else if (trim > 0 && ((size_t)pagesz + 17u) > trim) {
        LOG_ERROR("malloc page size %u vs trim threshold %u: the threshold is "
                "inert, the top is returned at %u instead\n",
                (unsigned)pagesz, (unsigned)trim, (unsigned)(pagesz + 17u));
    } else {
        LOG_INFO("malloc page size %u, trim threshold %u\n",
                (unsigned)pagesz, (unsigned)trim);
    }

    if (checkPeriod == 0) {
        return;
    }
    if (mTimHandle == nullptr) {
#if (configSUPPORT_STATIC_ALLOCATION == 1)
        mTimHandle = xTimerCreateStatic ("HeapStackMonitor", 1, pdTRUE,
                ( void * ) 0, Check, &mTimControlBlock);
#endif
    }

    if (mTimHandle != nullptr) {
        xTimerChangePeriod(mTimHandle, pdMS_TO_TICKS(checkPeriod), 0);
    }
}

void HeapStackMonitor::Stop()
{
    if (mTimHandle != nullptr) {
        if (xTimerIsTimerActive(mTimHandle) == pdTRUE) {
            xTimerStop(mTimHandle, 0);
        }
    }
}

uint32_t HeapStackMonitor::GetSysHeapThreshold()
{
    return mSysHeapTreshold;
}

void HeapStackMonitor::SetSysHeapThreshold(uint32_t th)
{
    mSysHeapTreshold = th;
}

uint32_t HeapStackMonitor::GetOsHeapThreshold()
{
    return mOsHeapTreshold;
}

void HeapStackMonitor::SetOsHeapThreshold(uint32_t th)
{
    mOsHeapTreshold = th;
}

uint32_t HeapStackMonitor::GetTaskStackThreshold()
{
    return mStackTreshold;
}

const volatile CrashHeapSample* HeapStackMonitor::GetHeapSample()
{
    return &mHeapSample;
}

void HeapStackMonitor::SetTaskStackThreshold(uint32_t th)
{
    mStackTreshold = th;
}

void HeapStackMonitor::Check(TimerHandle_t xTimer)
{
    uint32_t osFreeHeap = GetOsHeapFree();
    if (osFreeHeap < mOsHeapTreshold) {
        LOG_WARNING("FreeRTOS Heap: %d/%d bytes (min ever %d bytes)\n",
                osFreeHeap, GetOsHeapSize(),
                GetOsHeapWaterMark());
    }

    // One coherent set of system-heap figures per check. Free heap is
    // fordblks (free inside dlmalloc) plus the sbrk headroom (RAM never
    // handed to dlmalloc); the headroom alone is the fragmentation canary -
    // once it reaches 0 only fordblks-sized holes can serve new allocations.
    // Both read with the scheduler held, as the allocator's own lock does: an
    // allocation in between can grow the arena, moving bytes from the
    // headroom to fordblks, and the sum would come out low - and stay in
    // mSysHeapMinEver, which only goes down.
    vTaskSuspendAll();
    struct mallinfo mi = mallinfo();
    size_t sbrkHeadroom = Heap_getSbrkHeadroom();
    (void)xTaskResumeAll();
    size_t sysFreeHeap = (size_t)mi.fordblks + sbrkHeadroom;
    if (sysFreeHeap < mSysHeapMinEver) {
        mSysHeapMinEver = sysFreeHeap;  // true min-ever watermark (sampled)
    }

    uint32_t notifCount = HeapTelemetry::PhoneNotificationCount().load(std::memory_order_relaxed);
    uint32_t scanCount  = HeapTelemetry::AccessoryScanResultCount().load(std::memory_order_relaxed);
    uint32_t appBytes   = HeapTelemetry::AppAllocBytes().load(std::memory_order_relaxed);
    uint32_t appBlocks  = HeapTelemetry::AppAllocBlocks().load(std::memory_order_relaxed);

    // Publish the sample the crash dump embeds. The fault path only copies
    // these words (never computes them), so keep every store word-sized.
    mHeapSample.freeHeap      = (uint32_t)sysFreeHeap;
    mHeapSample.minEverFree   = (uint32_t)mSysHeapMinEver;
    mHeapSample.sbrkHeadroom  = (uint32_t)sbrkHeadroom;
    mHeapSample.fordblks      = (uint32_t)mi.fordblks;
    mHeapSample.ordblks       = (uint32_t)mi.ordblks;
    mHeapSample.notifCount    = notifCount;
    mHeapSample.scanCount     = scanCount;
    mHeapSample.appAllocBytes = appBytes;

    if (sysFreeHeap < mSysHeapTreshold) {
        // "min ever" is the true free-heap watermark; the sbrk headroom
        // (which the old line mislabeled as min ever) now goes by its real
        // name. The "System Heap: <free>/<total> bytes" prefix is parsed by
        // log parsers - extend only after it.
        LOG_WARNING("System Heap: %u/%u bytes (min ever %u, sbrk headroom %u)\n",
                (unsigned)sysFreeHeap, (unsigned)GetSysHeapSize(),
                (unsigned)mSysHeapMinEver, (unsigned)sbrkHeadroom);
    }

    // Dump the MSP residue whenever the main stack sets a new record. Gated
    // on a new peak rather than on the periodic cadence for two reasons: the
    // residue only changes when something goes deeper than before, so
    // repeating it would print the same words forever; and at the moment the
    // peak moves the words below it are guaranteed to be that excursion's,
    // which is exactly the evidence a stack-depth investigation needs. Costs nothing on an idle
    // watch - after the first few minutes the peak stops moving.
    const size_t isrUsed = Heap_isrStackHighWater();
    if (isrUsed > mIsrStackPeak) {
        mIsrStackPeak = isrUsed;
        DumpIsrStackResidue(isrUsed);
    }

    // Slow-cadence attribution line (evaluated every Nth check, 30 s at the
    // default 3 s period), gated on meaningful change since the last emitted
    // line so a genuinely idle watch stays silent instead of logging ~2880
    // identical lines a day into the ring and the syslog writer. It still
    // fires while anything is moving -- a creeping leak walks the free heap,
    // the min-ever watermark drops, or a consumer gauge changes -- so both
    // original purposes hold: a slow leak is visible long before the low-heap
    // warning, and the pre-OOM log still carries the consumer gauges. No idle
    // heartbeat by design; the crash-dump sample (published above) keeps
    // updating every check regardless.
    if ((mCheckCount++ % skInfoEveryNChecks) == 0) {
        size_t freeDelta = (sysFreeHeap > mLastInfoFreeHeap)
                         ? (sysFreeHeap - mLastInfoFreeHeap)
                         : (mLastInfoFreeHeap - sysFreeHeap);
        bool changed = !mLastInfoValid
                    || freeDelta >= skInfoFreeDeltaBytes
                    || mSysHeapMinEver < mLastInfoMinEver
                    || notifCount != mLastInfoNotifCount
                    || scanCount  != mLastInfoScanCount
                    || appBytes   != mLastInfoAppBytes
                    || appBlocks  != mLastInfoAppBlocks;
        if (changed) {
            // keepcost/uordblks/peak-sbrk are appended AFTER the existing fields
            // so the parsed prefix and every field ahead of them keep their
            // position. keepcost is the size of the top chunk: with it, a
            // reader can tell "top is clean and will trim" from "top is fenced
            // by a live block", which is the difference between a watch that
            // recovers on app exit and one that does not. "peak sbrk" is the
            // high-water of space ever taken from the system, tracked inside
            // _sbrk_r where the break actually moves -- exact, not sampled.
            // (mallinfo's usmblks cannot carry this: newlib documents it dead,
            // "unused -- always zero" in malloc.h. The internal
            // __malloc_max_sbrked_mem is maintained but never copied out.)
            //
            // The two-ended heap is why the field reads "peak used"
            // rather than "peak sbrk". Headroom no longer shrinks only when the
            // break rises: an image placed downward reserves the top of the
            // span and shrinks it too. So total minus the headroom watermark is
            // the high-water of everything ever committed AT ONCE from either
            // end, which is the quantity app launches actually turn on -- the old name
            // would now be reporting it under a label that is no longer true.
            //
            // "images" is the other half of that story: the reserved span right
            // now. Headroom + images + what sbrk has handed out is the total, so
            // a single line says which end the memory went to.
            LOG_INFO("System Heap: %u/%u bytes (min ever %u, sbrk headroom %u, "
                    "fordblks %u, ordblks %u, mallocs %u, mallocd %u, "
                    "notif %u, scan %u, app %u/%u, "
                    "keepcost %u, uordblks %u, peak used %u, images %u, "
                    "isr stack %u/%u)\n",   // used / MSPLIM, not the region
                    (unsigned)sysFreeHeap, (unsigned)GetSysHeapSize(),
                    (unsigned)mSysHeapMinEver, (unsigned)sbrkHeadroom,
                    (unsigned)mi.fordblks, (unsigned)mi.ordblks,
                    (unsigned)MallocCallCnt, (unsigned)TotalMallocdBytes,
                    (unsigned)notifCount, (unsigned)scanCount,
                    (unsigned)appBytes, (unsigned)appBlocks,
                    (unsigned)mi.keepcost, (unsigned)mi.uordblks,
                    (unsigned)(GetSysHeapSize() - Heap_getMinSbrkHeadroomEver()),
                    (unsigned)Heap_getImageReserved(),
                    (unsigned)Heap_isrStackHighWater(),
                    (unsigned)Heap_isrStackUsableSize());

            // Refresh the change-gate snapshot to this emitted line.
            mLastInfoFreeHeap   = sysFreeHeap;
            mLastInfoMinEver    = mSysHeapMinEver;
            mLastInfoNotifCount = notifCount;
            mLastInfoScanCount  = scanCount;
            mLastInfoAppBytes   = appBytes;
            mLastInfoAppBlocks  = appBlocks;
            mLastInfoValid      = true;
        }
    }

    size_t tasksNum = GetTasksInfo(mTaskInfoArray, skTasksMax);
    bool breakLine = false;
    for (uint32_t i = 0; i < tasksNum; i++) {
        if (mTaskInfoArray[i].xHandle != nullptr) {
            if (mTaskInfoArray[i].usStackHighWaterMark * 4 < mStackTreshold) {
                LOG_WARNING("Task %-15s Free stack %d bytes\n",
                        mTaskInfoArray[i].pcTaskName,
                        mTaskInfoArray[i].usStackHighWaterMark * 4);
                breakLine = true;
            }
        }
    }
    if (breakLine) {
        LOG_WARNING_WP("\n");
    }
}



