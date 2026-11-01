# Stability audit: experimental candidate

The `fix-atomic-allocation` candidate was installed on the Surface but has not been reported boot-tested. This subsequent optimization branch is not installed or boot-tested. Both are based on the driver used for the macOS install, not the later receive-lock experiment.

## Changes under evaluation

- MLAN allocations marked atomic use a preallocated, bounded pool instead of calling `IOMalloc` while preemption may be disabled. Oversize requests or pool exhaustion fail instead of sleeping. Pool capacity is provisional, not validated under real workloads.
- Heap-backed allocations freed by MLAN callbacks and packet completion are queued for release from a thread-call worker. This addresses queue cleanup that may run while the WMM spinlock is held. The worker uses `THREAD_CALL_OPTIONS_ONCE`; teardown assumes all MLAN producers have stopped before cancellation.
- Raw BAR register reads and writes through the user client require administrator privilege. The previously installed version has not received this restriction.
- This branch indexes active DMA mappings by address as well as retaining the shutdown list. Local simulated tests cover collisions, wrong-key removal, and concurrent insertion/removal; they do not test actual DMA or estimate Wi-Fi speed.
- DMA setup also checks that the entire mapped range fits in the 32-bit address space, including the end of the buffer; the boundary cases are checked locally.
- This branch serializes individual MLAN work items and timers with `THREAD_CALL_OPTIONS_ONCE`, so cancellation can wait for an in-flight callback even if another invocation is pending. A shared scheduling gate rejects MLAN work-item submissions once shutdown begins. The deferred-free worker and MLAN timer scheduling have separate lifecycles and still need a shutdown audit.
- The controller's firmware auto-start uses the same serialized-call option, and its retry is scheduled under the lock that sets the stopping flag. This closes a separate retry-versus-teardown race, subject to on-device verification.

The three host-side concurrency harnesses support `MARVELL_SANITIZER=thread` as well as their default AddressSanitizer/UndefinedBehaviorSanitizer configuration. Both modes passed locally. These harnesses replace kernel locks, callouts, and devices with user-space stand-ins; they cannot establish macOS or hardware stability.

## Outstanding risks and limitations

- **High priority: receive-work flush events are ignored.** `mlan_block_rx_process` can send `MLAN_EVENT_ID_DRV_FLUSH_RX_WORK` when receive processing is active, before reorder-table mutation or cleanup. The NXP Linux host waits for its receive workqueue; this host currently returns without waiting. That can allow reorder-table work to overlap receive processing. A safe implementation must account for callback context, avoid waiting on itself or while holding a required lock, and ensure no new RX work escapes the barrier. Do not assume the scheduling gate fixes this.
- The candidate and its worker shutdown sequence have not been tested in the macOS kernel, across association, sustained traffic, reconnect, sleep, or shutdown. Local tests replace the kernel locks and scheduler with user-space stubs.
- Work-item and timer stop/start sequencing still needs a concurrency audit; `thread_call_cancel_wait` is not a general barrier against later submissions, and timer destruction must not wait on a callback while holding a lock the callback needs. Interrupt callbacks and deferred frees must be fully quiesced before their context is destroyed.
- MLAN uses a mixture of allocation flags; other callbacks that can block while MLAN holds a spinlock still need a complete call-context audit, including DMA mapping and timer destruction. The atomic pool fixes the observed call site, not every possible lock violation.
- Scan selection reads MLAN-owned scan descriptors while taking a host lock. The host lock does not necessarily serialize mutations within MLAN; ownership and synchronization need further verification.
- Raw register access is privileged, but hardware-state lifetime and synchronization during device stop still require review. Avoid using diagnostic register writes while normal networking is active.
- The pool can run out or reject requests larger than 16 KiB. Recovery from these failures and normal operation at peak transmit queue depth need hardware testing and diagnostics.
- Transmit and receive both use a try-lock on the network state: contention deliberately drops frames, and transmit currently holds that lock while preparing a frame. Inspect `MarvellTxLockDrops` and `MarvellRxLockDrops` under sustained bidirectional traffic before deciding how to change interface lifetime and locking. The controller currently rejects Ethernet frames larger than 1514 bytes, including full-sized VLAN-tagged frames.
- The interface is Ethernet-shaped with a custom WPA2-PSK association helper; native macOS Wi-Fi settings, WPA3, enterprise authentication, and Bluetooth remain unimplemented.
- This is experimental kernel code. A backup boot entry without the driver and an offline transfer method are needed before attempting hardware validation.
