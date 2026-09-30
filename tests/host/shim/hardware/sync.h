/* Host stand-in for the Pico SDK's hardware/sync.h. */
#ifndef HOST_SHIM_HARDWARE_SYNC_H
#define HOST_SHIM_HARDWARE_SYNC_H
#define __dmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
