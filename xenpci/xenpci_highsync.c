/*
PV Drivers for Windows Xen HVM Domains

Copyright (c) 2014, James Harper
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of James Harper nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL JAMES HARPER BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "xenpci.h"

/* Not really necessary but keeps PREfast happy */
#if (VER_PRODUCTBUILD >= 7600)
static KDEFERRED_ROUTINE XenPci_HighSyncCallFunction0;
static KDEFERRED_ROUTINE XenPci_HighSyncCallFunctionN;
#endif

/*
we need these intrinsics as even going to HIGH_LEVEL doesn't ensure that interrupts are completely disabled
*/
#pragma intrinsic(_disable)
#pragma intrinsic(_enable)

struct {
  volatile ULONG        do_spin;
  volatile LONG         nr_procs_at_dispatch_level;
  volatile LONG         nr_spinning_at_sync_level;
  KDPC                  dpcs[MAX_VIRT_CPUS];
  KEVENT                highsync_complete_event;
  KIRQL                 sync_level;
  PXENPCI_HIGHSYNC_FUNCTION function0;
  PXENPCI_HIGHSYNC_FUNCTION functionN;  
  PVOID                 context;
} typedef highsync_info_t;

static VOID
XenPci_HighSyncCallFunction0(PRKDPC Dpc, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2) {
  highsync_info_t *highsync_info = Context;
  ULONG ActiveProcessorCount;
  KIRQL old_irql;
  
  UNREFERENCED_PARAMETER(Dpc);
  UNREFERENCED_PARAMETER(SystemArgument1);
  UNREFERENCED_PARAMETER(SystemArgument2);

  FUNCTION_ENTER();
#if (NTDDI_VERSION >= NTDDI_WINXP)
  ActiveProcessorCount = (ULONG)KeNumberProcessors;
#else
  ActiveProcessorCount = (ULONG)*KeNumberProcessors;
#endif
  InterlockedIncrement(&highsync_info->nr_procs_at_dispatch_level);
  if (highsync_info->sync_level > DISPATCH_LEVEL) {
    while (highsync_info->nr_procs_at_dispatch_level < (LONG)ActiveProcessorCount) {
      KeStallExecutionProcessor(1);
      KeMemoryBarrier();
    }
  }
  _disable(); //__asm cli;  
  KeRaiseIrql(highsync_info->sync_level, &old_irql);
  while (highsync_info->nr_spinning_at_sync_level < (LONG)ActiveProcessorCount - 1) {
    KeStallExecutionProcessor(1);
    KeMemoryBarrier();
  }
  highsync_info->function0(highsync_info->context);
  KeLowerIrql(old_irql);
  _enable(); //__asm sti;
  highsync_info->do_spin = FALSE;
  KeMemoryBarrier();  
  /* wait for all the other processors to complete spinning, just in case it matters */
  while (highsync_info->nr_spinning_at_sync_level) {
    KeStallExecutionProcessor(1);
    KeMemoryBarrier();
  }
  InterlockedDecrement(&highsync_info->nr_procs_at_dispatch_level);
  /* wait until nr_procs_at_dispatch_level drops to 0 indicating that nothing else requires highsync_info */
  while (highsync_info->nr_procs_at_dispatch_level) {
    KeStallExecutionProcessor(1);
    KeMemoryBarrier();
  }
  KeSetEvent(&highsync_info->highsync_complete_event, IO_NO_INCREMENT, FALSE);

  FUNCTION_EXIT();
}

static VOID
XenPci_HighSyncCallFunctionN(PRKDPC Dpc, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2) {
  highsync_info_t *highsync_info = Context;
  ULONG ActiveProcessorCount;
  KIRQL old_irql;
  
  UNREFERENCED_PARAMETER(Dpc);
  UNREFERENCED_PARAMETER(SystemArgument1);
  UNREFERENCED_PARAMETER(SystemArgument2);

  FUNCTION_ENTER();
  FUNCTION_MSG("(CPU = %d)\n", KeGetCurrentProcessorNumber());

  FUNCTION_MSG("CPU %d spinning...\n", KeGetCurrentProcessorNumber());
  InterlockedIncrement(&highsync_info->nr_procs_at_dispatch_level);
  if (highsync_info->sync_level > DISPATCH_LEVEL) {
#if (NTDDI_VERSION >= NTDDI_WINXP)
    ActiveProcessorCount = (ULONG)KeNumberProcessors;
#else
    ActiveProcessorCount = (ULONG)*KeNumberProcessors;
#endif
    while (highsync_info->nr_procs_at_dispatch_level < (LONG)ActiveProcessorCount) {
      KeStallExecutionProcessor(1);
      KeMemoryBarrier();
    }
  }
  _disable(); //__asm cli;  
  KeRaiseIrql(highsync_info->sync_level, &old_irql);
  InterlockedIncrement(&highsync_info->nr_spinning_at_sync_level);
  while(highsync_info->do_spin) {
    KeStallExecutionProcessor(1);
    KeMemoryBarrier();
  }
  highsync_info->functionN(highsync_info->context);
  KeLowerIrql(old_irql);
  _enable(); //__asm sti;
  InterlockedDecrement(&highsync_info->nr_spinning_at_sync_level);
  InterlockedDecrement(&highsync_info->nr_procs_at_dispatch_level);
  FUNCTION_EXIT();
  return;
}

VOID
XenPci_HighSync(PXENPCI_HIGHSYNC_FUNCTION function0, PXENPCI_HIGHSYNC_FUNCTION functionN, PVOID context) {
  ULONG ActiveProcessorCount;
  ULONG i;
  highsync_info_t *highsync_info;
  KIRQL old_irql;

  UNREFERENCED_PARAMETER(context);
  FUNCTION_ENTER();

  highsync_info = ExAllocatePoolWithTag(NonPagedPool, sizeof(highsync_info_t), XENPCI_POOL_TAG);
  RtlZeroMemory(highsync_info, sizeof(highsync_info_t));
  KeInitializeEvent(&highsync_info->highsync_complete_event, SynchronizationEvent, FALSE);
  highsync_info->function0 = function0;
  highsync_info->functionN = functionN;
  highsync_info->context = context;
  highsync_info->sync_level = HIGH_LEVEL;
  
#if (NTDDI_VERSION >= NTDDI_WINXP)
  ActiveProcessorCount = (ULONG)KeNumberProcessors;
#else
  ActiveProcessorCount = (ULONG)*KeNumberProcessors;
#endif

  /* Go to HIGH_LEVEL to prevent any races with Dpc's on the current processor */
  KeRaiseIrql(highsync_info->sync_level, &old_irql);

  highsync_info->do_spin = TRUE;
  for (i = 0; i < ActiveProcessorCount; i++) {
    if (i == 0)
      KeInitializeDpc(&highsync_info->dpcs[i], XenPci_HighSyncCallFunction0, highsync_info);
    else
      KeInitializeDpc(&highsync_info->dpcs[i], XenPci_HighSyncCallFunctionN, highsync_info);
    KeSetTargetProcessorDpc(&highsync_info->dpcs[i], (CCHAR)i);
    KeSetImportanceDpc(&highsync_info->dpcs[i], HighImportance);
    FUNCTION_MSG("queuing Dpc for CPU %d\n", i);
    KeInsertQueueDpc(&highsync_info->dpcs[i], NULL, NULL);
  }
  FUNCTION_MSG("All Dpc's queued\n");

  KeMemoryBarrier();
  KeLowerIrql(old_irql);

  FUNCTION_MSG("Waiting for highsync_complete_event\n");
  KeWaitForSingleObject(&highsync_info->highsync_complete_event, Executive, KernelMode, FALSE, NULL);
#if (NTDDI_VERSION >= NTDDI_WINXP)
  KeFlushQueuedDpcs();
#else
  {
    /* just wait 1 second until all DPC's finish - not ideal but it's only for W2K */
    LARGE_INTEGER interval;
    interval.QuadPart = -1 * 1000 * 1000 * 10; /* 1 second */
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
  }
#endif
  ExFreePoolWithTag(highsync_info, XENPCI_POOL_TAG);
  FUNCTION_EXIT();
}
