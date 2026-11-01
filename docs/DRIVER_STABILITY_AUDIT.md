# Stability audit: experimental candidate

This branch has not been boot-tested on the Surface. It is based on the last driver used for the macOS install, not the later receive-lock experiment.

## Changes under evaluation

- MLAN allocations marked atomic use a preallocated, bounded pool instead of calling `IOMalloc` while preemption may be disabled. Oversize requests or pool exhaustion fail instead of sleeping. Pool capacity is provisional, not validated under real workloads.
- Heap-backed allocations freed by MLAN callbacks and packet completion are queued for release from a thread-call worker. This addresses queue cleanup that may run while the WMM spinlock is held. The worker uses `THREAD_CALL_OPTIONS_ONCE`; teardown assumes all MLAN producers have stopped before cancellation.
- Raw BAR register reads and writes through the user client require administrator privilege. The previously installed version has not received this restriction.

## Outstanding risks and limitations

- The candidate and its worker shutdown sequence have not been tested in the macOS kernel, across association, sustained traffic, reconnect, sleep, or shutdown. Local tests replace the kernel locks and scheduler with user-space stubs.
- Several existing thread calls and timer callbacks still use the regular, non-serialized `thread_call_allocate`. Their shutdown and stop/start sequencing needs a separate concurrency audit; `thread_call_cancel_wait` alone is not a general barrier against later submissions.
- MLAN uses a mixture of allocation flags; other callbacks that can block while MLAN holds a spinlock still need a complete call-context audit, including DMA mapping and timer destruction. The atomic pool fixes the observed call site, not every possible lock violation.
- Scan selection reads MLAN-owned scan descriptors while taking a host lock. The host lock does not necessarily serialize mutations within MLAN; ownership and synchronization need further verification.
- The pool can run out or reject requests larger than 16 KiB. Recovery from these failures and normal operation at peak transmit queue depth need hardware testing and diagnostics.
- The interface is Ethernet-shaped with a custom WPA2-PSK association helper; native macOS Wi-Fi settings, WPA3, enterprise authentication, and Bluetooth remain unimplemented.
- This is experimental kernel code. A backup boot entry without the driver and an offline transfer method are needed before attempting hardware validation.
