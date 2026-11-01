// SPDX-License-Identifier: GPL-2.0
// Host callbacks for NXP's MLAN protocol core.
#include "MarvellW8897MlanHost.hpp"
#include "MarvellW8897Controller.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/mach_time.h>
#include <libkern/libkern.h>
#include <stdarg.h>

#define MLAN_64BIT 1
#define PCIE 1
#define PCIE8897 1
#define STA_SUPPORT 1
#define UAP_SUPPORT 1
#define DEBUG_LEVEL1 1
extern "C" {
#include "mlan.h"
}

namespace MarvellW8897MlanHost {

struct DmaMapping;
struct Context {
    MarvellW8897Controller* controller;
    mlan_device device;
    void* adapter;
    IOLock* mappingLock;
    DmaMapping* mappings;
    DmaMapping* reusableRxMappings;
    t_u32 reusableRxCount;
    t_u64 reusableRxHits;
    t_u64 rxMappingsCreated;
    thread_call_t mainCall;
    thread_call_t rxCall;
    thread_call_t txCall;
    thread_call_t commandCall;
    thread_call_t networkCall;
    thread_call_t scanReadCall;
    thread_call_t connectCall;
    thread_call_t connectTimeoutCall;
    uint32_t shuttingDown;
    t_u32 lastEventId;
    t_u64 eventCount;
    t_u32 hwSpecStatus;
    t_u32 initStatus;
    t_u32 initComplete;
    t_u32 associated;
    t_u32 macValid;
    t_u8 mac[MLAN_MAC_ADDR_LENGTH];
    IOLock* infoLock;
    mlan_ioctl_req infoRequest;
    mlan_ds_get_info infoData;
    t_u32 infoPhase; // 0 not requested, 1 pending, 2 ready, 3 failed.
    IOLock* scanLock;
    mlan_ioctl_req scanRequest;
    mlan_ds_scan scanData;
    mlan_ioctl_req scanReadRequest;
    mlan_ds_scan scanReadData;
    t_u32 scanPhase;
    t_u32 scanResultPhase;
    t_u32 scanStatus;
    t_u32 scanCount;
    IOLock* connectLock;
    mlan_ioctl_req connectRequest;
    mlan_ds_sec_cfg connectSecurity;
    mlan_ds_misc_cfg connectMisc;
    mlan_ds_bss connectBss;
    t_u8 connectSsid[MLAN_MAX_SSID_LENGTH];
    t_u8 connectPassphrase[MLAN_MAX_PASSPHRASE_LENGTH];
    t_u32 connectSsidLength;
    t_u32 connectPassphraseLength;
    t_u32 connectPhase;
    t_u32 connectPending;
    t_u32 connectRequestPhase;
    t_u32 connectStatus;
    bool connectPrefer5Attempted;
    bool connectPrefer5Disabled;
};

struct DmaMapping {
    DmaMapping* next;
    IOBufferMemoryDescriptor* buffer;
    IODMACommand* command;
    t_u8* caller;
    t_u8* bytes;
    t_u64 address;
    t_u32 size;
    t_u32 direction;
    bool owned;
    bool descriptorSet;
};

namespace {

bool isShuttingDown(const Context* host)
{ return __atomic_load_n(&host->shuttingDown, __ATOMIC_ACQUIRE) != 0; }

void beginShutdown(Context* host)
{ __atomic_store_n(&host->shuttingDown, 1U, __ATOMIC_RELEASE); }

struct Allocation {
    size_t total;
    uint64_t magic;
};
constexpr uint64_t kAllocationMagic = 0x4d4c414e38383937ULL;
constexpr t_u32 kDmaFromDevice = 2;

void* allocate(size_t size)
{
    if (size > static_cast<size_t>(-1) - sizeof(Allocation)) return nullptr;
    const size_t total = sizeof(Allocation) + size;
    auto* header = static_cast<Allocation*>(IOMalloc(total));
    if (!header) return nullptr;
    header->total = total;
    header->magic = kAllocationMagic;
    void* data = header + 1;
    bzero(data, size);
    return data;
}

void deallocate(void* data)
{
    if (!data) return;
    auto* header = static_cast<Allocation*>(data) - 1;
    if (header->magic != kAllocationMagic) {
        IOLog("MarvellW8897: MLAN invalid allocation header\n");
        return;
    }
    const size_t total = header->total;
    header->magic = 0;
    IOFree(header, total);
}

bool packetDataWithinAllocation(const mlan_buffer* packet)
{
    // All MLAN packet buffers supplied by this host use allocate(), whether
    // MLAN asks for a combined packet/data allocation or mallocs one itself.
    // Check the actual allocation before copying a firmware-supplied frame.
    const auto* header = reinterpret_cast<const Allocation*>(packet) - 1;
    if (header->magic != kAllocationMagic ||
        header->total < sizeof(Allocation) + sizeof(mlan_buffer))
        return false;

    const size_t allocationSize = header->total - sizeof(Allocation);
    const uintptr_t packetBase = reinterpret_cast<uintptr_t>(packet);
    const uintptr_t dataBase = reinterpret_cast<uintptr_t>(packet->pbuf);
    if (dataBase < packetBase || dataBase - packetBase > allocationSize)
        return false;

    const size_t available = allocationSize - (dataBase - packetBase);
    return packet->data_offset <= available &&
           packet->data_len <= available - packet->data_offset;
}

mlan_status mallocCallback(t_void*, t_u32 size, t_u32, t_u8** out)
{
    if (!out) return MLAN_STATUS_FAILURE;
    *out = static_cast<t_u8*>(allocate(size));
    return *out ? MLAN_STATUS_SUCCESS : MLAN_STATUS_FAILURE;
}

mlan_status freeCallback(t_void*, t_u8* data)
{
    deallocate(data);
    return MLAN_STATUS_SUCCESS;
}

t_void* memsetCallback(t_void*, t_void* dest, t_u8 byte, t_u32 count)
{
    memset(dest, byte, count);
    return dest;
}

t_void* memcpyCallback(t_void*, t_void* dest, const t_void* source, t_u32 count)
{
    bcopy(source, dest, count);
    return dest;
}

t_void* memcpyExtCallback(t_void* host, t_void* dest, const t_void* source,
                          t_u32 count, t_u32 capacity)
{
    // Match NXP MOAL's bounded-copy contract: callers often ignore the
    // return value and expect the portion that fits to be populated.
    if (!dest || !source || count == 0 || capacity == 0) return dest;
    return memcpyCallback(host, dest, source,
                          count < capacity ? count : capacity);
}

t_void* memmoveCallback(t_void* host, t_void* dest, const t_void* source, t_u32 count)
{
    return memcpyCallback(host, dest, source, count);
}

t_s32 memcmpCallback(t_void*, const t_void* left, const t_void* right, t_u32 count)
{
    return memcmp(left, right, count);
}

void delayCallback(t_void*, t_u32 microseconds)
{
    if (microseconds >= 1000) IOSleep((microseconds + 999) / 1000);
    else IODelay(microseconds);
}

void sleepRangeCallback(t_void* host, t_u32 minimum, t_u32)
{
    delayCallback(host, minimum);
}

mlan_status bootTimeCallback(t_void*, t_u64* nanoseconds)
{
    if (!nanoseconds) return MLAN_STATUS_FAILURE;
    uint64_t continuous = mach_continuous_time();
    absolutetime_to_nanoseconds(continuous, nanoseconds);
    return MLAN_STATUS_SUCCESS;
}

mlan_status systemTimeCallback(t_void*, t_u32* seconds, t_u32* microseconds)
{
    if (!seconds || !microseconds) return MLAN_STATUS_FAILURE;
    clock_sec_t sec = 0;
    clock_usec_t usec = 0;
    clock_get_calendar_microtime(&sec, &usec);
    *seconds = static_cast<t_u32>(sec);
    *microseconds = static_cast<t_u32>(usec);
    return MLAN_STATUS_SUCCESS;
}

mlan_status hostTimeCallback(t_u64* nanoseconds)
{
    return bootTimeCallback(nullptr, nanoseconds);
}

mlan_status initLockCallback(t_void*, t_void** out)
{
    if (!out) return MLAN_STATUS_FAILURE;
    *out = IOSimpleLockAlloc();
    return *out ? MLAN_STATUS_SUCCESS : MLAN_STATUS_FAILURE;
}

mlan_status freeLockCallback(t_void*, t_void* lock)
{
    if (lock) IOSimpleLockFree(static_cast<IOSimpleLock*>(lock));
    return MLAN_STATUS_SUCCESS;
}

mlan_status spinLockCallback(t_void*, t_void* lock)
{
    if (!lock) return MLAN_STATUS_FAILURE;
    IOSimpleLockLock(static_cast<IOSimpleLock*>(lock));
    return MLAN_STATUS_SUCCESS;
}

mlan_status spinUnlockCallback(t_void*, t_void* lock)
{
    if (!lock) return MLAN_STATUS_FAILURE;
    IOSimpleLockUnlock(static_cast<IOSimpleLock*>(lock));
    return MLAN_STATUS_SUCCESS;
}

struct Timer {
    thread_call_t call;
    IOSimpleLock* lock;
    void (*callback)(t_void*);
    t_void* context;
    t_u32 intervalMs;
    bool active;
    bool periodic;
};

void timerFired(thread_call_param_t arg, thread_call_param_t)
{
    auto* timer = static_cast<Timer*>(arg);
    IOSimpleLockLock(timer->lock);
    const bool active = timer->active;
    if (!timer->periodic) timer->active = false;
    IOSimpleLockUnlock(timer->lock);
    if (!active) return;
    timer->callback(timer->context);
    IOSimpleLockLock(timer->lock);
    if (timer->active && timer->periodic) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(timer->intervalMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(timer->call, deadline);
    }
    IOSimpleLockUnlock(timer->lock);
}

mlan_status initTimerCallback(t_void*, t_void** out,
                              t_void (*callback)(t_void*), t_void* context)
{
    if (!out || !callback) return MLAN_STATUS_FAILURE;
    *out = nullptr;
    auto* timer = static_cast<Timer*>(allocate(sizeof(Timer)));
    if (!timer) return MLAN_STATUS_FAILURE;
    timer->lock = IOSimpleLockAlloc();
    timer->callback = callback;
    timer->context = context;
    timer->call = thread_call_allocate(timerFired, timer);
    if (!timer->lock || !timer->call) {
        if (timer->call) thread_call_free(timer->call);
        if (timer->lock) IOSimpleLockFree(timer->lock);
        deallocate(timer);
        return MLAN_STATUS_FAILURE;
    }
    *out = timer;
    return MLAN_STATUS_SUCCESS;
}

mlan_status stopTimerCallback(t_void*, t_void* value)
{
    if (!value) return MLAN_STATUS_FAILURE;
    auto* timer = static_cast<Timer*>(value);
    IOSimpleLockLock(timer->lock);
    timer->active = false;
    IOSimpleLockUnlock(timer->lock);
    // MLAN may stop a timer while holding a lock needed by its callback.
    // Waiting here can deadlock; only timer destruction waits for completion.
    thread_call_cancel(timer->call);
    return MLAN_STATUS_SUCCESS;
}

mlan_status freeTimerCallback(t_void* host, t_void* value)
{
    if (!value) return MLAN_STATUS_SUCCESS;
    auto* timer = static_cast<Timer*>(value);
    stopTimerCallback(host, value);
    thread_call_cancel_wait(timer->call);
    thread_call_free(timer->call);
    IOSimpleLockFree(timer->lock);
    deallocate(timer);
    return MLAN_STATUS_SUCCESS;
}

mlan_status startTimerCallback(t_void* host, t_void* value, t_u8 periodic, t_u32 ms)
{
    if (!value || ms == 0) return MLAN_STATUS_FAILURE;
    auto* timer = static_cast<Timer*>(value);
    stopTimerCallback(host, value);
    IOSimpleLockLock(timer->lock);
    timer->active = true;
    timer->periodic = periodic != 0;
    timer->intervalMs = ms;
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    thread_call_enter_delayed(timer->call, deadline);
    IOSimpleLockUnlock(timer->lock);
    return MLAN_STATUS_SUCCESS;
}

mlan_status readRegCallback(t_void* value, t_u32 reg, t_u32* out)
{
    auto* host = static_cast<Context*>(value);
    if (!host || !out || !host->controller) return MLAN_STATUS_FAILURE;
    return host->controller->readReg32FromBar(2, reg, *out)
        ? MLAN_STATUS_SUCCESS : MLAN_STATUS_FAILURE;
}

mlan_status writeRegCallback(t_void* value, t_u32 reg, t_u32 data)
{
    auto* host = static_cast<Context*>(value);
    if (!host || !host->controller) return MLAN_STATUS_FAILURE;
    return host->controller->writeReg32ToBar(2, reg, data)
        ? MLAN_STATUS_SUCCESS : MLAN_STATUS_FAILURE;
}

mlan_status allocBufferCallback(t_void*, t_u32 size, ppmlan_buffer out)
{
    if (!out || size > static_cast<t_u32>(-1) - sizeof(mlan_buffer))
        return MLAN_STATUS_FAILURE;
    auto* buffer = static_cast<pmlan_buffer>(allocate(sizeof(mlan_buffer) + size));
    if (!buffer) return MLAN_STATUS_FAILURE;
    buffer->pbuf = reinterpret_cast<t_u8*>(buffer + 1);
    *out = buffer;
    return MLAN_STATUS_SUCCESS;
}

mlan_status freeBufferCallback(t_void*, pmlan_buffer buffer)
{
    deallocate(buffer);
    return MLAN_STATUS_SUCCESS;
}

mlan_status unsupported(t_void*, t_u32, t_u32, t_u8*) { return MLAN_STATUS_FAILURE; }
mlan_status unsupportedData(t_void*, pmlan_buffer, t_u32, t_u32) { return MLAN_STATUS_FAILURE; }
void releaseMapping(DmaMapping* mapping)
{
    if (!mapping) return;
    if (mapping->command) {
        if (mapping->descriptorSet)
            mapping->command->clearMemoryDescriptor(true);
        mapping->command->release();
    }
    if (mapping->buffer) mapping->buffer->release();
    deallocate(mapping);
}

DmaMapping* createMapping(t_u32 size)
{
    if (size == 0) return nullptr;
    auto* mapping = static_cast<DmaMapping*>(allocate(sizeof(DmaMapping)));
    if (!mapping) return nullptr;
    mapping->size = size;
    mapping->buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        size, 0xffffffffULL);
    if (!mapping->buffer) {
        releaseMapping(mapping);
        return nullptr;
    }
    mapping->bytes = static_cast<t_u8*>(mapping->buffer->getBytesNoCopy());
    if (!mapping->bytes) {
        releaseMapping(mapping);
        return nullptr;
    }
    mapping->buffer->setLength(size);
    mapping->command = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 32, size,
        IODMACommand::kMapped, size, 256);
    if (!mapping->command ||
        mapping->command->setMemoryDescriptor(mapping->buffer, true) != kIOReturnSuccess) {
        releaseMapping(mapping);
        return nullptr;
    }
    mapping->descriptorSet = true;
    UInt64 offset = 0;
    IODMACommand::Segment64 segment {};
    UInt32 count = 1;
    if (mapping->command->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
        count != 1 || segment.fLength < size || segment.fIOVMAddr > 0xffffffffULL) {
        releaseMapping(mapping);
        return nullptr;
    }
    mapping->address = segment.fIOVMAddr;
    return mapping;
}

void addMapping(Context* host, DmaMapping* mapping)
{
    IOLockLock(host->mappingLock);
    mapping->next = host->mappings;
    host->mappings = mapping;
    IOLockUnlock(host->mappingLock);
}

DmaMapping* takeMapping(Context* host, t_u8* bytes, t_u64 address, t_u32 size, bool owned)
{
    if (!host || !host->mappingLock) return nullptr;
    IOLockLock(host->mappingLock);
    DmaMapping** link = &host->mappings;
    while (*link && ((*link)->caller != bytes || (*link)->address != address ||
                     (*link)->size != size || (*link)->owned != owned)) {
        link = &(*link)->next;
    }
    DmaMapping* found = *link;
    if (found) *link = found->next;
    IOLockUnlock(host->mappingLock);
    return found;
}

// Keep a bounded pool of prepared RX bounce buffers. The 8897 replaces its
// receive ring buffer after each packet, so otherwise every packet allocates
// and prepares a new IODMACommand and physically contiguous buffer.
DmaMapping* takeReusableRxMapping(Context* host, t_u32 size)
{
    IOLockLock(host->mappingLock);
    DmaMapping** link = &host->reusableRxMappings;
    while (*link && (*link)->size != size) link = &(*link)->next;
    DmaMapping* mapping = *link;
    if (mapping) {
        *link = mapping->next;
        --host->reusableRxCount;
        __atomic_fetch_add(&host->reusableRxHits, 1ULL, __ATOMIC_RELAXED);
    }
    IOLockUnlock(host->mappingLock);
    return mapping;
}

bool cacheRxMapping(Context* host, DmaMapping* mapping)
{
    bool cached = false;
    IOLockLock(host->mappingLock);
    if (!isShuttingDown(host) && host->reusableRxCount < 128) {
        mapping->caller = nullptr;
        mapping->next = host->reusableRxMappings;
        host->reusableRxMappings = mapping;
        ++host->reusableRxCount;
        cached = true;
    }
    IOLockUnlock(host->mappingLock);
    return cached;
}

void releaseCachedRxMappings(Context* host)
{
    while (host->reusableRxMappings) {
        DmaMapping* mapping = host->reusableRxMappings;
        host->reusableRxMappings = mapping->next;
        releaseMapping(mapping);
    }
    host->reusableRxCount = 0;
}

mlan_status allocDmaCallback(t_void* value, t_u32 size, t_u8** out, t_u64* address)
{
    auto* host = static_cast<Context*>(value);
    if (!host || !out || !address) return MLAN_STATUS_FAILURE;
    *out = nullptr;
    *address = 0;
    DmaMapping* mapping = createMapping(size);
    if (!mapping) return MLAN_STATUS_FAILURE;
    mapping->owned = true;
    mapping->caller = mapping->bytes;
    addMapping(host, mapping);
    *out = mapping->bytes;
    *address = mapping->address;
    return MLAN_STATUS_SUCCESS;
}

mlan_status freeDmaCallback(t_void* value, t_u32 size, t_u8* bytes, t_u64 address)
{
    auto* mapping = takeMapping(static_cast<Context*>(value), bytes, address, size, true);
    if (!mapping) return MLAN_STATUS_FAILURE;
    releaseMapping(mapping);
    return MLAN_STATUS_SUCCESS;
}

mlan_status mapCallback(t_void* value, t_u8* bytes, t_u64* address,
                        t_u32 size, t_u32 direction)
{
    auto* host = static_cast<Context*>(value);
    if (!host || !bytes || !address || direction > 2) return MLAN_STATUS_FAILURE;
    *address = 0;
    DmaMapping* mapping = direction == kDmaFromDevice
        ? takeReusableRxMapping(host, size) : nullptr;
    if (!mapping) {
        mapping = createMapping(size);
        if (mapping && direction == kDmaFromDevice)
            __atomic_fetch_add(&host->rxMappingsCreated, 1ULL, __ATOMIC_RELAXED);
    }
    if (!mapping) return MLAN_STATUS_FAILURE;
    mapping->owned = false;
    mapping->caller = bytes;
    mapping->direction = direction;
    if (direction == 0 || direction == 1) {
        bcopy(bytes, mapping->bytes, size);
        if (mapping->command->synchronize(kIODirectionOut) != kIOReturnSuccess) {
            releaseMapping(mapping);
            return MLAN_STATUS_FAILURE;
        }
    }
    addMapping(host, mapping);
    *address = mapping->address;
    return MLAN_STATUS_SUCCESS;
}

mlan_status unmapCallback(t_void* value, t_u8* bytes, t_u64 address,
                          t_u32 size, t_u32 direction)
{
    auto* mapping = takeMapping(static_cast<Context*>(value), bytes, address, size, false);
    if (!mapping) return MLAN_STATUS_FAILURE;
    mlan_status result = MLAN_STATUS_SUCCESS;
    if (direction != mapping->direction) result = MLAN_STATUS_FAILURE;
    if (mapping->direction == 0 || mapping->direction == 2) {
        if (mapping->command->synchronize(kIODirectionIn) != kIOReturnSuccess)
            result = MLAN_STATUS_FAILURE;
        else bcopy(mapping->bytes, bytes, size);
    }
    if (result != MLAN_STATUS_SUCCESS || mapping->direction != kDmaFromDevice ||
        !cacheRxMapping(static_cast<Context*>(value), mapping))
        releaseMapping(mapping);
    return result;
}

mlan_status syncDmaCallback(t_void* value, t_u32 size, t_u64 address,
                            moal_dma_sync_direction_t direction, bool toDevice)
{
    auto* host = static_cast<Context*>(value);
    if (!host || !host->mappingLock || direction > MOAL_DMA_SYNC_FROM_DEVICE)
        return MLAN_STATUS_FAILURE;
    IOLockLock(host->mappingLock);
    DmaMapping* mapping = host->mappings;
    while (mapping && (mapping->address != address || size > mapping->size))
        mapping = mapping->next;
    if (!mapping) {
        IOLockUnlock(host->mappingLock);
        return MLAN_STATUS_FAILURE;
    }
    if (toDevice && !mapping->owned) bcopy(mapping->caller, mapping->bytes, size);
    const IOReturn status = mapping->command->synchronize(
        toDevice ? kIODirectionOut : kIODirectionIn);
    if (status == kIOReturnSuccess && !toDevice && !mapping->owned)
        bcopy(mapping->bytes, mapping->caller, size);
    IOLockUnlock(host->mappingLock);
    return status == kIOReturnSuccess ? MLAN_STATUS_SUCCESS : MLAN_STATUS_FAILURE;
}

mlan_status syncToCpuCallback(t_void* host, t_u32 size, t_u64 address,
                              moal_dma_sync_direction_t direction)
{ return syncDmaCallback(host, size, address, direction, false); }

mlan_status syncToDeviceCallback(t_void* host, t_u32 size, t_u64 address,
                                 moal_dma_sync_direction_t direction)
{ return syncDmaCallback(host, size, address, direction, true); }

mlan_status completionHw(t_void* value, mlan_status status, pmlan_hw_info, pmlan_bss_tbl)
{
    auto* host = static_cast<Context*>(value);
    if (!host) return MLAN_STATUS_FAILURE;
    __atomic_store_n(&host->hwSpecStatus, static_cast<t_u32>(status), __ATOMIC_RELEASE);
    IOLog("MarvellW8897: MLAN hardware-spec status=%u\n", static_cast<unsigned>(status));
    return MLAN_STATUS_SUCCESS;
}
mlan_status completionInit(t_void* value, mlan_status status)
{
    auto* host = static_cast<Context*>(value);
    if (!host) return MLAN_STATUS_FAILURE;
    __atomic_store_n(&host->initStatus, static_cast<t_u32>(status), __ATOMIC_RELEASE);
    __atomic_store_n(&host->initComplete, 1U, __ATOMIC_RELEASE);
    if (status == MLAN_STATUS_SUCCESS && host->networkCall)
        thread_call_enter(host->networkCall);
    IOLog("MarvellW8897: MLAN firmware initialization status=%u\n",
          static_cast<unsigned>(status));
    return MLAN_STATUS_SUCCESS;
}
mlan_status completionShutdown(t_void*, mlan_status) { return MLAN_STATUS_SUCCESS; }
mlan_status completionPacket(t_void* value, pmlan_buffer packet, mlan_status)
{
    auto* host = static_cast<Context*>(value);
    if (!host || !packet || packet->pdesc != host) return MLAN_STATUS_FAILURE;
    deallocate(packet);
    return MLAN_STATUS_SUCCESS;
}
mlan_status completionReceive(t_void*, pmlan_buffer, t_u32, mlan_status)
{ return MLAN_STATUS_SUCCESS; } // PCIe RX buffers are completed by MLAN itself.
mlan_status completionReceivePacket(t_void* value, pmlan_buffer packet)
{
    auto* host = static_cast<Context*>(value);
    if (!host || !host->controller || !packet || !packet->pbuf ||
        packet->data_len < 14 || packet->data_len > 4096 ||
        !packetDataWithinAllocation(packet) || isShuttingDown(host))
        return MLAN_STATUS_FAILURE;
    return host->controller->receiveEthernetFrame(
        packet->pbuf + packet->data_offset, packet->data_len)
        ? MLAN_STATUS_SUCCESS : MLAN_STATUS_FAILURE;
}
void networkCallFired(thread_call_param_t value, thread_call_param_t)
{
    auto* host = static_cast<Context*>(value);
    if (!host || isShuttingDown(host) || !host->controller || !host->adapter ||
        __atomic_load_n(&host->initComplete, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_load_n(&host->initStatus, __ATOMIC_ACQUIRE) != MLAN_STATUS_SUCCESS)
        return;
    if (__atomic_load_n(&host->macValid, __ATOMIC_ACQUIRE) == 0) {
        bool submit = false;
        IOLockLock(host->infoLock);
        if (host->infoPhase == 0) {
            bzero(&host->infoRequest, sizeof(host->infoRequest));
            bzero(&host->infoData, sizeof(host->infoData));
            host->infoData.sub_command = MLAN_OID_GET_FW_INFO;
            host->infoRequest.bss_index = 0;
            host->infoRequest.req_id = MLAN_IOCTL_GET_INFO;
            host->infoRequest.action = MLAN_ACT_GET;
            host->infoRequest.pbuf = reinterpret_cast<t_u8*>(&host->infoData);
            host->infoRequest.buf_len = sizeof(host->infoData);
            host->infoPhase = 1;
            submit = true;
        }
        if (host->infoPhase != 2) {
            IOLockUnlock(host->infoLock);
            if (submit) {
                const mlan_status status = mlan_ioctl(host->adapter, &host->infoRequest);
                if (status == MLAN_STATUS_PENDING) {
                    thread_call_enter(host->mainCall);
                } else {
                    IOLockLock(host->infoLock);
                    if (host->infoPhase == 1)
                        host->infoPhase = status == MLAN_STATUS_SUCCESS ? 2 : 3;
                    IOLockUnlock(host->infoLock);
                    if (status == MLAN_STATUS_SUCCESS)
                        thread_call_enter(host->networkCall);
                    else
                        IOLog("MarvellW8897: firmware-info request failed, status=%u\n",
                              static_cast<unsigned>(status));
                }
            }
            return;
        }
        const t_u8* mac = host->infoData.param.fw_info.mac_addr;
        bool allZero = true;
        for (unsigned i = 0; i < MLAN_MAC_ADDR_LENGTH; ++i)
            allZero &= mac[i] == 0;
        if (allZero || (mac[0] & 1U) != 0) {
            host->infoPhase = 3;
            IOLockUnlock(host->infoLock);
            IOLog("MarvellW8897: firmware returned an invalid MAC address\n");
            return;
        }
        bcopy(mac, host->mac, MLAN_MAC_ADDR_LENGTH);
        const t_u16 fwBands = host->infoData.param.fw_info.fw_bands;
        const t_u16 region = host->infoData.param.fw_info.region_code;
        __atomic_store_n(&host->macValid, 1U, __ATOMIC_RELEASE);
        IOLockUnlock(host->infoLock);
        host->controller->setProperty("MarvellFirmwareBands", fwBands, 16);
        host->controller->setProperty("MarvellFirmwareRegion", region, 16);
    }
    host->controller->mlanCoreReady(
        host->mac, __atomic_load_n(&host->associated, __ATOMIC_ACQUIRE) != 0);
}
void scanReadCallFired(thread_call_param_t value, thread_call_param_t)
{
    auto* host = static_cast<Context*>(value);
    if (!host || isShuttingDown(host) || !host->adapter || !host->scanLock) return;
    IOLockLock(host->scanLock);
    if (host->scanPhase != 2 || host->scanStatus != MLAN_STATUS_SUCCESS ||
        host->scanResultPhase != 0) {
        IOLockUnlock(host->scanLock);
        return;
    }
    host->scanResultPhase = 1;
    bzero(&host->scanReadRequest, sizeof(host->scanReadRequest));
    bzero(&host->scanReadData, sizeof(host->scanReadData));
    host->scanReadData.sub_command = MLAN_OID_SCAN_NORMAL;
    host->scanReadRequest.bss_index = 0;
    host->scanReadRequest.req_id = MLAN_IOCTL_SCAN;
    host->scanReadRequest.action = MLAN_ACT_GET;
    host->scanReadRequest.pbuf = reinterpret_cast<t_u8*>(&host->scanReadData);
    host->scanReadRequest.buf_len = sizeof(host->scanReadData);
    IOLockUnlock(host->scanLock);

    const mlan_status status = mlan_ioctl(host->adapter, &host->scanReadRequest);
    if (status == MLAN_STATUS_PENDING) {
        thread_call_enter(host->mainCall);
        return;
    }
    IOLockLock(host->scanLock);
    if (host->scanResultPhase == 1) {
        if (status == MLAN_STATUS_SUCCESS)
            host->scanCount = host->scanReadData.param.scan_resp.num_in_scan_table;
        else
            host->scanStatus = static_cast<t_u32>(status);
        host->scanResultPhase = 2;
    }
    IOLockUnlock(host->scanLock);
    if (host->connectCall) thread_call_enter(host->connectCall);
}

// 1 scanning, 2 clear old IE, 3 auth, 4 cipher, 5 passphrase,
// 6 embedded supplicant, 7 RSN IE, 8 find BSS, 9 start BSS,
// 10 awaiting connected event, 11 connected, 12 failed.
void connectTimeoutFired(thread_call_param_t value, thread_call_param_t)
{
    auto* host = static_cast<Context*>(value);
    if (!host || isShuttingDown(host) || !host->connectLock) return;
    bool timedOut = false;
    bool retryOn2GHz = false;
    IOLockLock(host->connectLock);
    if (host->connectPhase == 10 &&
        __atomic_load_n(&host->associated, __ATOMIC_ACQUIRE) == 0) {
        host->connectPending = 0;
        if (host->connectPrefer5Attempted && !host->connectPrefer5Disabled) {
            host->connectPrefer5Disabled = true;
            host->connectPrefer5Attempted = false;
            host->connectPhase = 8;
            host->connectStatus = MLAN_STATUS_PENDING;
            retryOn2GHz = true;
        } else {
            host->connectPhase = 12;
            host->connectStatus = MLAN_STATUS_FAILURE;
            bzero(host->connectPassphrase, sizeof(host->connectPassphrase));
            timedOut = true;
        }
    }
    IOLockUnlock(host->connectLock);
    if (retryOn2GHz && host->connectCall) thread_call_enter(host->connectCall);
    if (timedOut) IOLog("MarvellW8897: association timed out\n");
}

void finishConnectStep(Context* host, mlan_status status, t_u32 expectedPhase)
{
    if (!host || !host->connectLock) return;
    bool next = false;
    bool awaitEvent = false;
    t_u16 selectedChannel = 0;
    IOLockLock(host->connectLock);
    if (host->connectPhase == expectedPhase && expectedPhase >= 2 &&
        expectedPhase <= 9 && host->connectPending &&
        host->connectRequestPhase == expectedPhase) {
        const t_u32 oldPhase = host->connectPhase;
        host->connectPending = 0;
        host->connectStatus = static_cast<t_u32>(status);
        if (oldPhase == 5) {
            bzero(host->connectPassphrase, sizeof(host->connectPassphrase));
            bzero(&host->connectSecurity, sizeof(host->connectSecurity));
        }
        if (status == MLAN_STATUS_SUCCESS) {
            if (oldPhase == 8)
                selectedChannel = host->connectBss.param.ssid_bssid.channel;
            host->connectPhase = oldPhase == 9 ? 10 : oldPhase + 1;
            next = oldPhase != 9;
            awaitEvent = oldPhase == 9;
        } else if ((oldPhase == 8 || oldPhase == 9) &&
                   host->connectPrefer5Attempted &&
                   !host->connectPrefer5Disabled) {
            host->connectPrefer5Disabled = true;
            host->connectPrefer5Attempted = false;
            host->connectPhase = 8;
            host->connectStatus = MLAN_STATUS_PENDING;
            next = true;
        } else {
            host->connectPhase = 12;
            bzero(host->connectPassphrase, sizeof(host->connectPassphrase));
        }
    }
    IOLockUnlock(host->connectLock);
    if (selectedChannel && host->controller)
        host->controller->setProperty("MarvellAssociatedChannel",
                                      selectedChannel, 16);
    if (next && !isShuttingDown(host)) thread_call_enter(host->connectCall);
    if (awaitEvent && !isShuttingDown(host) && host->connectTimeoutCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(30000, kMillisecondScale, &deadline);
        thread_call_enter_delayed(host->connectTimeoutCall, deadline);
    }
}

void connectCallFired(thread_call_param_t value, thread_call_param_t)
{
    auto* host = static_cast<Context*>(value);
    if (!host || isShuttingDown(host) || !host->adapter || !host->connectLock)
        return;
    IOLockLock(host->connectLock);
    if (host->connectPhase == 1) {
        unsigned long long scan[4] = {};
        if (!getScanStatus(host, scan) || scan[0] != 2 ||
            (scan[1] == MLAN_STATUS_SUCCESS && scan[2] != 2)) {
            IOLockUnlock(host->connectLock);
            return;
        }
        if (scan[1] != MLAN_STATUS_SUCCESS || scan[3] == 0) {
            host->connectStatus = MLAN_STATUS_FAILURE;
            host->connectPhase = 12;
            bzero(host->connectPassphrase, sizeof(host->connectPassphrase));
            IOLockUnlock(host->connectLock);
            return;
        }
        host->connectPhase = 2;
    }
    if (host->connectPhase < 2 || host->connectPhase > 9 ||
        host->connectPending) {
        IOLockUnlock(host->connectLock);
        return;
    }
    if (host->connectPhase == 5 && host->connectPassphraseLength == 0)
        host->connectPhase = 6;
    if (host->connectPhase == 7 && host->connectPassphraseLength == 0)
        host->connectPhase = 8;

    const t_u32 phase = host->connectPhase;
    mlan_ssid_bssid foundBss {};
    t_u8 preferredBssid[MLAN_MAC_ADDR_LENGTH] = {};
    t_u16 preferredChannel = 0;
    t_u32 fiveGHzCount = 0;
    t_u32 visibleFiveGHzCount = 0;
    if (phase == 8 && host->scanLock) {
        IOLockLock(host->scanLock);
        const auto& response = host->scanReadData.param.scan_resp;
        // NXP allocates MRVDRV_MAX_BSSID_LIST (200) scan descriptors.
        constexpr t_u32 kMaxScanEntries = 200;
        if (host->scanResultPhase == 2 &&
            host->scanStatus == MLAN_STATUS_SUCCESS &&
            response.pscan_table && host->scanCount <= kMaxScanEntries) {
            const auto* entries = reinterpret_cast<const BSSDescriptor_t*>(
                response.pscan_table);
            t_u8 bestSignal = 0;
            for (t_u32 i = 0; i < host->scanCount; ++i) {
                const auto& entry = entries[i];
                if (entry.channel > 14 && entry.channel <= 196)
                    ++visibleFiveGHzCount;
                if (entry.channel == 0 || entry.channel > 196 ||
                    entry.ssid.ssid_len != host->connectSsidLength ||
                    memcmp(entry.ssid.ssid, host->connectSsid,
                           host->connectSsidLength) != 0)
                    continue;
                if (entry.channel > 14) ++fiveGHzCount;
                const bool wantedBand = host->connectPrefer5Disabled
                    ? entry.channel <= 14 : entry.channel > 14;
                if (!wantedBand) continue;
                const t_u8 signal = SCAN_RSSI(entry.rssi);
                if (!preferredChannel || signal > bestSignal) {
                    bestSignal = signal;
                    preferredChannel = static_cast<t_u16>(entry.channel);
                    bcopy(entry.mac_address, preferredBssid,
                          sizeof(preferredBssid));
                }
            }
        }
        IOLockUnlock(host->scanLock);
    }
    if (phase == 9)
        foundBss = host->connectBss.param.ssid_bssid;
    bzero(&host->connectRequest, sizeof(host->connectRequest));
    bzero(&host->connectSecurity, sizeof(host->connectSecurity));
    bzero(&host->connectMisc, sizeof(host->connectMisc));
    bzero(&host->connectBss, sizeof(host->connectBss));
    auto& req = host->connectRequest;
    req.bss_index = 0;
    req.action = MLAN_ACT_SET;
    if (phase == 2 || phase == 7) {
        req.req_id = MLAN_IOCTL_MISC_CFG;
        req.pbuf = reinterpret_cast<t_u8*>(&host->connectMisc);
        req.buf_len = sizeof(host->connectMisc);
        auto& ie = host->connectMisc.param.gen_ie;
        host->connectMisc.sub_command = MLAN_OID_MISC_GEN_IE;
        ie.type = MLAN_IE_TYPE_GEN_IE;
        if (phase == 7) {
            // RSN v1, CCMP group/pairwise, PSK AKM, no optional capabilities.
            static const t_u8 kWpa2PskRsn[] = {
                0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
                0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
            };
            ie.len = sizeof(kWpa2PskRsn);
            bcopy(kWpa2PskRsn, ie.ie_data, sizeof(kWpa2PskRsn));
        }
    } else if (phase == 3 || phase == 4 || phase == 5 || phase == 6) {
        req.req_id = MLAN_IOCTL_SEC_CFG;
        req.pbuf = reinterpret_cast<t_u8*>(&host->connectSecurity);
        req.buf_len = sizeof(host->connectSecurity);
        auto& sec = host->connectSecurity;
        if (phase == 3) {
            sec.sub_command = MLAN_OID_SEC_CFG_AUTH_MODE;
            sec.param.auth_mode = MLAN_AUTH_MODE_OPEN;
        } else if (phase == 4) {
            sec.sub_command = MLAN_OID_SEC_CFG_ENCRYPT_MODE;
            sec.param.encrypt_mode = host->connectPassphraseLength
                ? MLAN_ENCRYPTION_MODE_CCMP : MLAN_ENCRYPTION_MODE_NONE;
        } else if (phase == 5) {
            sec.sub_command = MLAN_OID_SEC_CFG_PASSPHRASE;
            auto& pass = sec.param.passphrase;
            pass.ssid.ssid_len = host->connectSsidLength;
            bcopy(host->connectSsid, pass.ssid.ssid, host->connectSsidLength);
            pass.psk_type = MLAN_PSK_PASSPHRASE;
            pass.psk.passphrase.passphrase_len = host->connectPassphraseLength;
            bcopy(host->connectPassphrase, pass.psk.passphrase.passphrase,
                  host->connectPassphraseLength);
        } else {
            sec.sub_command = MLAN_OID_SEC_CFG_EWPA_ENABLED;
            sec.param.ewpa_enabled = host->connectPassphraseLength ? 1 : 0;
        }
    } else {
        req.req_id = MLAN_IOCTL_BSS;
        req.pbuf = reinterpret_cast<t_u8*>(&host->connectBss);
        req.buf_len = sizeof(host->connectBss);
        req.action = phase == 8 ? MLAN_ACT_GET : MLAN_ACT_SET;
        host->connectBss.sub_command = phase == 8
            ? MLAN_OID_BSS_FIND_BSS : MLAN_OID_BSS_START;
        auto& bss = host->connectBss.param.ssid_bssid;
        if (phase == 9) {
            bss = foundBss;
        } else {
            bss.ssid.ssid_len = host->connectSsidLength;
            bcopy(host->connectSsid, bss.ssid.ssid, host->connectSsidLength);
            if (phase == 8 && preferredChannel) {
                bcopy(preferredBssid, bss.bssid, sizeof(preferredBssid));
                host->connectPrefer5Attempted = !host->connectPrefer5Disabled;
            }
        }
    }
    host->connectPending = 1;
    host->connectRequestPhase = phase;
    const t_u16 publishedPreferredChannel = preferredChannel;
    IOLockUnlock(host->connectLock);
    if (phase == 8 && host->controller) {
        host->controller->setProperty("MarvellScan5GHzCount", fiveGHzCount, 32);
        host->controller->setProperty("MarvellScanAll5GHzCount", visibleFiveGHzCount, 32);
        host->controller->setProperty("MarvellPreferredChannel",
                                      publishedPreferredChannel, 16);
    }

    const mlan_status status = mlan_ioctl(host->adapter, &host->connectRequest);
    if (status == MLAN_STATUS_PENDING) {
        thread_call_enter(host->mainCall);
        return;
    }
    finishConnectStep(host, status, phase);
}
void mainCallFired(thread_call_param_t value, thread_call_param_t)
{
    auto* host = static_cast<Context*>(value);
    if (!isShuttingDown(host) && host->adapter) mlan_main_process(host->adapter);
}
void rxCallFired(thread_call_param_t value, thread_call_param_t)
{
    auto* host = static_cast<Context*>(value);
    if (!isShuttingDown(host) && host->adapter)
        mlan_process_pcie_interrupt_cb(host->adapter, RX_DATA);
}
void txCallFired(thread_call_param_t value, thread_call_param_t)
{
    auto* host = static_cast<Context*>(value);
    if (!isShuttingDown(host) && host->adapter)
        mlan_process_pcie_interrupt_cb(host->adapter, TX_COMPLETE);
}
void commandCallFired(thread_call_param_t value, thread_call_param_t)
{
    auto* host = static_cast<Context*>(value);
    if (!isShuttingDown(host) && host->adapter)
        mlan_process_pcie_interrupt_cb(host->adapter, RX_CMD_RESP);
}
mlan_status completionEvent(t_void* value, pmlan_event event)
{
    auto* host = static_cast<Context*>(value);
    if (!host || !event || isShuttingDown(host)) return MLAN_STATUS_FAILURE;
    __atomic_store_n(&host->lastEventId, static_cast<t_u32>(event->event_id), __ATOMIC_RELAXED);
    __atomic_fetch_add(&host->eventCount, 1ULL, __ATOMIC_RELAXED);
    switch (event->event_id) {
    case MLAN_EVENT_ID_DRV_DEFER_HANDLING:
        thread_call_enter(host->mainCall);
        break;
    case MLAN_EVENT_ID_DRV_DEFER_RX_WORK:
    case MLAN_EVENT_ID_DRV_DEFER_RX_DATA:
        thread_call_enter(host->rxCall);
        break;
    case MLAN_EVENT_ID_DRV_DEFER_TX_COMPLTE:
    case MLAN_EVENT_ID_DRV_DELAY_TX_COMPLETE:
        thread_call_enter(host->txCall);
        break;
    case MLAN_EVENT_ID_DRV_DEFER_CMDRESP:
        thread_call_enter(host->commandCall);
        break;
    case MLAN_EVENT_ID_DRV_CONNECTED:
        __atomic_store_n(&host->associated, 1U, __ATOMIC_RELEASE);
        if (host->networkCall) thread_call_enter(host->networkCall);
        if (host->connectLock) {
            IOLockLock(host->connectLock);
            if (host->connectPhase >= 9 && host->connectPhase <= 12) {
                host->connectPhase = 11;
                host->connectStatus = MLAN_STATUS_SUCCESS;
                host->connectPending = 0;
                bzero(host->connectPassphrase, sizeof(host->connectPassphrase));
            }
            IOLockUnlock(host->connectLock);
        }
        break;
    case MLAN_EVENT_ID_FW_DISCONNECTED:
        __atomic_store_n(&host->associated, 0U, __ATOMIC_RELEASE);
        if (host->networkCall) thread_call_enter(host->networkCall);
        if (host->connectLock) {
            bool retryOn2GHz = false;
            IOLockLock(host->connectLock);
            if (host->connectPhase >= 9 && host->connectPhase <= 11) {
                host->connectPending = 0;
                if (host->connectPhase == 10 &&
                    host->connectPrefer5Attempted &&
                    !host->connectPrefer5Disabled) {
                    host->connectPrefer5Disabled = true;
                    host->connectPrefer5Attempted = false;
                    host->connectPhase = 8;
                    host->connectStatus = MLAN_STATUS_PENDING;
                    retryOn2GHz = true;
                } else {
                    host->connectPhase = 12;
                    host->connectStatus = MLAN_STATUS_FAILURE;
                    bzero(host->connectPassphrase,
                          sizeof(host->connectPassphrase));
                }
            }
            IOLockUnlock(host->connectLock);
            if (retryOn2GHz) {
                if (host->connectTimeoutCall) thread_call_cancel(host->connectTimeoutCall);
                if (host->connectCall) thread_call_enter(host->connectCall);
            }
        }
        break;
    default:
        break;
    }
    return MLAN_STATUS_SUCCESS;
}
mlan_status completionIoctl(t_void* value, pmlan_ioctl_req request, mlan_status status)
{
    auto* host = static_cast<Context*>(value);
    if (!host || !request || !host->scanLock) return MLAN_STATUS_FAILURE;
    if (request == &host->infoRequest) {
        IOLockLock(host->infoLock);
        if (host->infoPhase == 1)
            host->infoPhase = status == MLAN_STATUS_SUCCESS ? 2 : 3;
        IOLockUnlock(host->infoLock);
        if (status == MLAN_STATUS_SUCCESS && !isShuttingDown(host))
            thread_call_enter(host->networkCall);
        else if (status != MLAN_STATUS_SUCCESS)
            IOLog("MarvellW8897: firmware-info completion failed, status=%u\n",
                  static_cast<unsigned>(status));
        return MLAN_STATUS_SUCCESS;
    }
    bool readResults = false;
    bool connectAdvanced = false;
    IOLockLock(host->scanLock);
    if (request == &host->scanRequest && host->scanPhase == 1) {
        host->scanStatus = static_cast<t_u32>(status);
        host->scanPhase = 2;
        readResults = status == MLAN_STATUS_SUCCESS;
    } else if (request == &host->scanReadRequest && host->scanResultPhase == 1) {
        if (status == MLAN_STATUS_SUCCESS)
            host->scanCount = host->scanReadData.param.scan_resp.num_in_scan_table;
        else
            host->scanStatus = static_cast<t_u32>(status);
        host->scanResultPhase = 2;
        connectAdvanced = true;
    }
    IOLockUnlock(host->scanLock);
    if (readResults && !isShuttingDown(host)) thread_call_enter(host->scanReadCall);
    if (!readResults && request == &host->scanRequest &&
        !isShuttingDown(host) && host->connectCall)
        thread_call_enter(host->connectCall);
    if (connectAdvanced && !isShuttingDown(host) && host->connectCall)
        thread_call_enter(host->connectCall);
    if (request == &host->connectRequest)
        finishConnectStep(host, status, host->connectRequestPhase);
    return MLAN_STATUS_SUCCESS;
}
mlan_status vdllData(t_void*, t_u32, t_u8*) { return MLAN_STATUS_FAILURE; }
void printCallback(t_void*, t_u32, char* format, ...)
{
    va_list args;
    va_start(args, format);
    IOLogv(format, args);
    va_end(args);
}
void assertCallback(t_void*, t_u32 condition)
{
    if (!condition) IOLog("MarvellW8897: MLAN assertion failed\n");
}
void histCallback(t_void*, t_u32, t_u16, t_s8, t_s8, t_u8) {}
void peerCallback(t_void*, t_u32, t_u8*, t_s8, t_s8) {}
void tpCallback(t_void*, t_void*, t_u32) {}
void tpRxCallback(t_void*, unsigned int, unsigned int) {}
void amsduTpCallback(t_void*, t_s32, t_s32) {}
t_u64 divideCallback(t_u64 numerator, t_u32 denominator)
{ return denominator ? numerator / denominator : 0; }
t_u32 crc32Le(t_u32 initial, const t_u8* data, unsigned long length)
{
    t_u32 crc = initial;
    for (unsigned long i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return crc;
}
t_u32 crc32Be(t_u32 initial, const t_u8* data, unsigned long length)
{
    t_u32 crc = initial;
    for (unsigned long i = 0; i < length; ++i) {
        crc ^= static_cast<t_u32>(data[i]) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80000000U) ? 0x04c11db7U : 0U);
    }
    return crc;
}
mlan_status shortSsidCallback(t_u8* ssid, t_u32 length, t_u32* out)
{
    if (!ssid || !length || !out) return MLAN_STATUS_FAILURE;
    *out = ~crc32Le(~0U, ssid, length);
    return MLAN_STATUS_SUCCESS;
}
t_u16 read16(const void* source) { t_u16 value; bcopy(source, &value, sizeof(value)); return value; }
t_u32 read32(const void* source) { t_u32 value; bcopy(source, &value, sizeof(value)); return value; }
void write16(void* target, t_u16 value) { bcopy(&value, target, sizeof(value)); }
void write32(void* target, t_u32 value) { bcopy(&value, target, sizeof(value)); }

} // namespace

Context* registerCore(MarvellW8897Controller* controller)
{
    if (!controller) return nullptr;
    auto* host = static_cast<Context*>(IOMalloc(sizeof(Context)));
    if (!host) return nullptr;
    bzero(host, sizeof(*host));
    host->controller = controller;
    host->mappingLock = IOLockAlloc();
    host->infoLock = IOLockAlloc();
    host->scanLock = IOLockAlloc();
    host->connectLock = IOLockAlloc();
    if (!host->mappingLock || !host->infoLock || !host->scanLock || !host->connectLock) {
        if (host->connectLock) IOLockFree(host->connectLock);
        if (host->scanLock) IOLockFree(host->scanLock);
        if (host->infoLock) IOLockFree(host->infoLock);
        if (host->mappingLock) IOLockFree(host->mappingLock);
        IOFree(host, sizeof(*host));
        return nullptr;
    }
    host->mainCall = thread_call_allocate(mainCallFired, host);
    host->rxCall = thread_call_allocate(rxCallFired, host);
    host->txCall = thread_call_allocate(txCallFired, host);
    host->commandCall = thread_call_allocate(commandCallFired, host);
    host->networkCall = thread_call_allocate(networkCallFired, host);
    host->scanReadCall = thread_call_allocate(scanReadCallFired, host);
    host->connectCall = thread_call_allocate(connectCallFired, host);
    host->connectTimeoutCall = thread_call_allocate(connectTimeoutFired, host);
    if (!host->mainCall || !host->rxCall || !host->txCall ||
        !host->commandCall || !host->networkCall || !host->scanReadCall ||
        !host->connectCall || !host->connectTimeoutCall) {
        beginShutdown(host);
        if (host->mainCall) thread_call_free(host->mainCall);
        if (host->rxCall) thread_call_free(host->rxCall);
        if (host->txCall) thread_call_free(host->txCall);
        if (host->commandCall) thread_call_free(host->commandCall);
        if (host->networkCall) thread_call_free(host->networkCall);
        if (host->scanReadCall) thread_call_free(host->scanReadCall);
        if (host->connectCall) thread_call_free(host->connectCall);
        if (host->connectTimeoutCall) thread_call_free(host->connectTimeoutCall);
        IOLockFree(host->connectLock);
        IOLockFree(host->scanLock);
        IOLockFree(host->infoLock);
        IOLockFree(host->mappingLock);
        IOFree(host, sizeof(*host));
        return nullptr;
    }
    mlan_device& device = host->device;
    device.pmoal_handle = host;
    device.card_type = CARD_TYPE_PCIE8897;
    // Match the NXP Linux host default: retain capabilities advertised by
    // firmware. A zero mask strips every low capability bit, leaving 11b only.
    device.dev_cap_mask = 0xffffffffU;
    // MLAN copies this into rx_buf_size before creating the PCIe RX ring.
    // The NXP Linux host defaults to 4 KiB when 8 KiB AMSDU RX is disabled.
    device.amsdu_rx_size = MLAN_RX_DATA_BUF_SIZE_4K;
    device.indication_gpio = 0xff;
    // Keep the radio awake while Recovery downloads the installer. MLAN's
    // default PSP mode can add latency and reduce sustained receive throughput.
    device.ps_mode = MLAN_INIT_PARA_DISABLED;
    // This Surface's Linux log disables deep sleep for PCI revision 0.
    // NXP MLAN otherwise enables auto deep sleep by default.
    device.auto_ds = MLAN_INIT_PARA_DISABLED;
    device.bss_attr[0].bss_type = MLAN_BSS_TYPE_STA;
    device.bss_attr[0].frame_type = MLAN_DATA_FRAME_TYPE_ETH_II;
    device.bss_attr[0].active = 1;

    mlan_callbacks& c = device.callbacks;
    c.moal_get_fw_data = unsupported;
    c.moal_get_vdll_data = vdllData;
    c.moal_get_hw_spec_complete = completionHw;
    c.moal_init_fw_complete = completionInit;
    c.moal_shutdown_fw_complete = completionShutdown;
    c.moal_send_packet_complete = completionPacket;
    c.moal_recv_complete = completionReceive;
    c.moal_recv_packet = completionReceivePacket;
    c.moal_recv_event = completionEvent;
    c.moal_ioctl_complete = completionIoctl;
    c.moal_alloc_mlan_buffer = allocBufferCallback;
    c.moal_free_mlan_buffer = freeBufferCallback;
    c.moal_write_reg = writeRegCallback;
    c.moal_read_reg = readRegCallback;
    c.moal_write_data_sync = unsupportedData;
    c.moal_read_data_sync = unsupportedData;
    c.moal_malloc = mallocCallback;
    c.moal_mfree = freeCallback;
    c.moal_malloc_consistent = allocDmaCallback;
    c.moal_mfree_consistent = freeDmaCallback;
    c.moal_malloc_cached = allocDmaCallback;
    c.moal_mfree_cached = freeDmaCallback;
    c.moal_dma_sync_to_cpu = syncToCpuCallback;
    c.moal_dma_sync_to_device = syncToDeviceCallback;
    c.moal_map_memory = mapCallback;
    c.moal_unmap_memory = unmapCallback;
    c.moal_memset = memsetCallback;
    c.moal_memcpy = memcpyCallback;
    c.moal_memcpy_ext = memcpyExtCallback;
    c.moal_memmove = memmoveCallback;
    c.moal_memcmp = memcmpCallback;
    c.moal_udelay = delayCallback;
    c.moal_usleep_range = sleepRangeCallback;
    c.moal_get_boot_ktime = bootTimeCallback;
    c.moal_get_system_time = systemTimeCallback;
    c.moal_init_timer = initTimerCallback;
    c.moal_free_timer = freeTimerCallback;
    c.moal_start_timer = startTimerCallback;
    c.moal_stop_timer = stopTimerCallback;
    c.moal_init_lock = initLockCallback;
    c.moal_free_lock = freeLockCallback;
    c.moal_spin_lock = spinLockCallback;
    c.moal_spin_unlock = spinUnlockCallback;
    c.moal_print = printCallback;
    c.moal_assert = assertCallback;
    c.moal_hist_data_add = histCallback;
    c.moal_updata_peer_signal = peerCallback;
    c.moal_get_host_time_ns = hostTimeCallback;
    c.moal_do_div = divideCallback;
    c.moal_tp_accounting = tpCallback;
    c.moal_tp_accounting_rx_param = tpRxCallback;
    c.moal_amsdu_tp_accounting = amsduTpCallback;
    c.moal_calc_short_ssid = shortSsidCallback;
    c.moal_crc32_be = crc32Be;
    c.moal_unaligned_access.moal_read_u16 = read16;
    c.moal_unaligned_access.moal_read_u32 = read32;
    c.moal_unaligned_access.moal_write_u16 = write16;
    c.moal_unaligned_access.moal_write_u32 = write32;

    if (mlan_register(&device, &host->adapter) != MLAN_STATUS_SUCCESS || !host->adapter) {
        IOLog("MarvellW8897: MLAN core registration failed\n");
        beginShutdown(host);
        thread_call_cancel_wait(host->mainCall);
        thread_call_cancel_wait(host->rxCall);
        thread_call_cancel_wait(host->txCall);
        thread_call_cancel_wait(host->commandCall);
        thread_call_cancel_wait(host->networkCall);
        thread_call_cancel_wait(host->scanReadCall);
        thread_call_cancel_wait(host->connectCall);
        thread_call_cancel_wait(host->connectTimeoutCall);
        thread_call_free(host->mainCall);
        thread_call_free(host->rxCall);
        thread_call_free(host->txCall);
        thread_call_free(host->commandCall);
        thread_call_free(host->networkCall);
        thread_call_free(host->scanReadCall);
        thread_call_free(host->connectCall);
        thread_call_free(host->connectTimeoutCall);
        while (host->mappings) {
            DmaMapping* mapping = host->mappings;
            host->mappings = mapping->next;
            releaseMapping(mapping);
        }
        releaseCachedRxMappings(host);
        IOLockFree(host->mappingLock);
        IOLockFree(host->infoLock);
        IOLockFree(host->scanLock);
        IOLockFree(host->connectLock);
        IOFree(host, sizeof(*host));
        return nullptr;
    }
    IOLog("MarvellW8897: NXP MLAN PCIe 8897 core registered\n");
    return host;
}

void unregisterCore(Context* host)
{
    if (!host) return;
    beginShutdown(host);
    thread_call_cancel_wait(host->mainCall);
    thread_call_cancel_wait(host->rxCall);
    thread_call_cancel_wait(host->txCall);
    thread_call_cancel_wait(host->commandCall);
    thread_call_cancel_wait(host->networkCall);
    thread_call_cancel_wait(host->scanReadCall);
    thread_call_cancel_wait(host->connectCall);
    thread_call_cancel_wait(host->connectTimeoutCall);
    if (host->adapter) mlan_unregister(host->adapter);
    thread_call_free(host->mainCall);
    thread_call_free(host->rxCall);
    thread_call_free(host->txCall);
    thread_call_free(host->commandCall);
    thread_call_free(host->networkCall);
    thread_call_free(host->scanReadCall);
    thread_call_free(host->connectCall);
    thread_call_free(host->connectTimeoutCall);
    while (host->mappings) {
        DmaMapping* mapping = host->mappings;
        host->mappings = mapping->next;
        releaseMapping(mapping);
    }
    releaseCachedRxMappings(host);
    IOLockFree(host->mappingLock);
    IOLockFree(host->infoLock);
    IOLockFree(host->scanLock);
    IOLockFree(host->connectLock);
    IOFree(host, sizeof(*host));
}

unsigned startFirmware(Context* host, bool msiInterrupt)
{
    if (!host || !host->adapter || isShuttingDown(host))
        return static_cast<unsigned>(MLAN_STATUS_FAILURE);
    mlan_set_int_mode(host->adapter,
                      msiInterrupt ? PCIE_INT_MODE_MSI : PCIE_INT_MODE_LEGACY, 0);
    mlan_init_param parameters {};
    if (mlan_set_init_param(host->adapter, &parameters) != MLAN_STATUS_SUCCESS)
        return static_cast<unsigned>(MLAN_STATUS_FAILURE);
    const mlan_status status = mlan_init_fw(host->adapter);
    if (status == MLAN_STATUS_PENDING) thread_call_enter(host->mainCall);
    else if (status == MLAN_STATUS_SUCCESS &&
             __atomic_load_n(&host->initComplete, __ATOMIC_ACQUIRE) == 0)
        // MLAN can finish synchronously; there will be no later completion
        // callback to publish the interface in that case.
        completionInit(host, MLAN_STATUS_SUCCESS);
    return static_cast<unsigned>(status);
}

bool processInterrupt(Context* host)
{
    if (!host || !host->adapter || isShuttingDown(host)) return false;
    if (mlan_interrupt(0xffff, host->adapter) == MLAN_STATUS_FAILURE) return false;
    thread_call_enter(host->mainCall);
    return true;
}

bool getStatus(Context* host, unsigned long long out[6])
{
    if (!host || !out) return false;
    if (host->controller) {
        host->controller->setProperty("MarvellRxMappingReuseHits",
            __atomic_load_n(&host->reusableRxHits, __ATOMIC_RELAXED), 64);
        host->controller->setProperty("MarvellRxMappingsCreated",
            __atomic_load_n(&host->rxMappingsCreated, __ATOMIC_RELAXED), 64);
    }
    out[0] = host->adapter ? 1 : 0;
    out[1] = __atomic_load_n(&host->initComplete, __ATOMIC_ACQUIRE);
    out[2] = __atomic_load_n(&host->initStatus, __ATOMIC_ACQUIRE);
    out[3] = __atomic_load_n(&host->hwSpecStatus, __ATOMIC_ACQUIRE);
    out[4] = __atomic_load_n(&host->lastEventId, __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&host->eventCount, __ATOMIC_RELAXED);
    return true;
}

bool sendEthernetFrame(Context* host, mbuf_t source, unsigned length)
{
    if (!host || !host->adapter || !source || length < 14 || length > 1514 ||
        isShuttingDown(host) ||
        __atomic_load_n(&host->initComplete, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_load_n(&host->initStatus, __ATOMIC_ACQUIRE) != MLAN_STATUS_SUCCESS ||
        __atomic_load_n(&host->associated, __ATOMIC_ACQUIRE) == 0)
        return false;

    constexpr unsigned kAlignment = DMA_ALIGNMENT;
    constexpr unsigned kHeadroom = MLAN_MIN_DATA_HEADER_LEN;
    const size_t allocation = sizeof(mlan_buffer) + kAlignment + kHeadroom + length;
    auto* packet = static_cast<pmlan_buffer>(allocate(allocation));
    if (!packet) return false;
    uintptr_t start = reinterpret_cast<uintptr_t>(packet + 1);
    start = (start + kAlignment - 1) & ~(static_cast<uintptr_t>(kAlignment) - 1);
    packet->pbuf = reinterpret_cast<t_u8*>(start);
    packet->data_offset = kHeadroom;
    packet->data_len = length;
    packet->buf_type = MLAN_BUF_TYPE_DATA;
    packet->bss_index = 0;
    packet->pdesc = host;
    if (mbuf_copydata(source, 0, length, packet->pbuf + packet->data_offset) != 0) {
        deallocate(packet);
        return false;
    }
    const mlan_status status = mlan_send_packet(host->adapter, packet);
    if (status == MLAN_STATUS_PENDING) {
        thread_call_enter(host->mainCall);
        return true;
    }
    deallocate(packet);
    return false;
}

unsigned requestScan(Context* host, const unsigned char* ssid, unsigned length)
{
    if (!host || !host->adapter || !host->scanLock || length > MLAN_MAX_SSID_LENGTH ||
        (length != 0 && !ssid) || isShuttingDown(host) ||
        __atomic_load_n(&host->initComplete, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_load_n(&host->initStatus, __ATOMIC_ACQUIRE) != MLAN_STATUS_SUCCESS)
        return 2;

    IOLockLock(host->scanLock);
    if (host->scanPhase == 1 || host->scanResultPhase == 1) {
        IOLockUnlock(host->scanLock);
        return 1;
    }
    host->scanPhase = 1;
    host->scanResultPhase = 0;
    host->scanStatus = MLAN_STATUS_PENDING;
    host->scanCount = 0;
    bzero(&host->scanRequest, sizeof(host->scanRequest));
    bzero(&host->scanData, sizeof(host->scanData));
    host->scanData.sub_command = length ? MLAN_OID_SCAN_SPECIFIC_SSID : MLAN_OID_SCAN_NORMAL;
    if (length) {
        host->scanData.param.scan_req.scan_ssid.ssid_len = length;
        bcopy(ssid, host->scanData.param.scan_req.scan_ssid.ssid, length);
    }
    host->scanRequest.bss_index = 0;
    host->scanRequest.req_id = MLAN_IOCTL_SCAN;
    host->scanRequest.action = MLAN_ACT_SET;
    host->scanRequest.pbuf = reinterpret_cast<t_u8*>(&host->scanData);
    host->scanRequest.buf_len = sizeof(host->scanData);
    IOLockUnlock(host->scanLock);

    const mlan_status status = mlan_ioctl(host->adapter, &host->scanRequest);
    if (status == MLAN_STATUS_PENDING) {
        thread_call_enter(host->mainCall);
        return 0;
    }
    bool readResults = false;
    IOLockLock(host->scanLock);
    if (host->scanPhase == 1) {
        host->scanStatus = static_cast<t_u32>(status);
        host->scanPhase = 2;
        readResults = status == MLAN_STATUS_SUCCESS;
    }
    IOLockUnlock(host->scanLock);
    if (readResults) thread_call_enter(host->scanReadCall);
    return status == MLAN_STATUS_SUCCESS ? 0 : 3;
}

bool getScanStatus(Context* host, unsigned long long out[4])
{
    if (!host || !host->scanLock || !out) return false;
    IOLockLock(host->scanLock);
    out[0] = host->scanPhase;
    out[1] = host->scanStatus;
    out[2] = host->scanResultPhase;
    out[3] = host->scanCount;
    IOLockUnlock(host->scanLock);
    return true;
}

unsigned requestConnect(Context* host, const unsigned char* ssid, unsigned ssidLength,
                        const unsigned char* passphrase, unsigned passphraseLength)
{
    if (!host || !host->adapter || !host->connectLock || !ssid ||
        ssidLength == 0 || ssidLength > MLAN_MAX_SSID_LENGTH ||
        (passphraseLength != 0 &&
         (passphraseLength < MLAN_MIN_PASSPHRASE_LENGTH ||
          passphraseLength > MLAN_MAX_PASSPHRASE_LENGTH || !passphrase)) ||
        isShuttingDown(host) ||
        __atomic_load_n(&host->initComplete, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_load_n(&host->initStatus, __ATOMIC_ACQUIRE) != MLAN_STATUS_SUCCESS)
        return 2;

    IOLockLock(host->connectLock);
    if ((host->connectPhase >= 1 && host->connectPhase <= 10) ||
        __atomic_load_n(&host->associated, __ATOMIC_ACQUIRE) != 0) {
        IOLockUnlock(host->connectLock);
        return 1;
    }
    bzero(host->connectSsid, sizeof(host->connectSsid));
    bzero(host->connectPassphrase, sizeof(host->connectPassphrase));
    bcopy(ssid, host->connectSsid, ssidLength);
    if (passphraseLength)
        bcopy(passphrase, host->connectPassphrase, passphraseLength);
    host->connectSsidLength = ssidLength;
    host->connectPassphraseLength = passphraseLength;
    host->connectStatus = MLAN_STATUS_PENDING;
    host->connectPhase = 1;
    host->connectPending = 0;
    host->connectPrefer5Attempted = false;
    host->connectPrefer5Disabled = false;
    IOLockUnlock(host->connectLock);

    const unsigned scan = requestScan(host, ssid, ssidLength);
    if (scan != 0) {
        IOLockLock(host->connectLock);
        host->connectPhase = scan == 1 ? 0 : 12;
        host->connectStatus = MLAN_STATUS_FAILURE;
        bzero(host->connectPassphrase, sizeof(host->connectPassphrase));
        IOLockUnlock(host->connectLock);
        return scan;
    }
    return 0;
}

bool getConnectStatus(Context* host, unsigned long long out[4])
{
    if (!host || !host->connectLock || !out) return false;
    IOLockLock(host->connectLock);
    out[0] = host->connectPhase;
    out[1] = host->connectStatus;
    out[2] = host->connectPending;
    out[3] = __atomic_load_n(&host->associated, __ATOMIC_ACQUIRE);
    IOLockUnlock(host->connectLock);
    return true;
}

} // namespace MarvellW8897MlanHost
