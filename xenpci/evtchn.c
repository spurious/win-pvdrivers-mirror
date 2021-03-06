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
static KDEFERRED_ROUTINE EvtChn_DpcBounce;
#endif

#if defined(_X86_)
  #define xchg(p1, p2) InterlockedExchange(p1, p2)
  #define synch_clear_bit(p1, p2) InterlockedBitTestAndReset(p2, p1)
  #define synch_set_bit(p1, p2) InterlockedBitTestAndSet(p2, p1)
  #define bit_scan_forward(p1, p2) _BitScanForward(p1, p2)
#else
  #define xchg(p1, p2) InterlockedExchange64(p1, p2)
  #define synch_clear_bit(p1, p2) InterlockedBitTestAndReset64(p2, p1)
  #define synch_set_bit(p1, p2) InterlockedBitTestAndSet64(p2, p1)
  #define bit_scan_forward(p1, p2) _BitScanForward64(p1, p2)
#endif

#define BITS_PER_LONG (sizeof(xen_ulong_t) * 8)
#define BITS_PER_LONG_SHIFT (5 + (sizeof(xen_ulong_t) >> 3))

static VOID
EvtChn_DpcBounce(PRKDPC Dpc, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2) {
  ev_action_t *action = Context;

  UNREFERENCED_PARAMETER(Dpc);
  UNREFERENCED_PARAMETER(SystemArgument1);
  UNREFERENCED_PARAMETER(SystemArgument2);

  //FUNCTION_ENTER();

  if (action->type != EVT_ACTION_TYPE_EMPTY) { 
    action->ServiceRoutine(action->ServiceContext);
  }
  //FUNCTION_EXIT();
}

#if 0
/* Called at DIRQL */
BOOLEAN
EvtChn_AckEvent(PVOID context, evtchn_port_t port, BOOLEAN *last_interrupt) {
  PXENPCI_DEVICE_DATA xpdd = context;
  ULONG pcpu = KeGetCurrentProcessorNumber() & 0xff;
  ULONG evt_word;
  ULONG evt_bit;
  xen_ulong_t val;
  int i;
  
  evt_bit = port & (BITS_PER_LONG - 1);
  evt_word = port >> BITS_PER_LONG_SHIFT;

  val = synch_clear_bit(evt_bit, (volatile xen_long_t *)&xpdd->evtchn_pending_pvt[pcpu][evt_word]);
  *last_interrupt = TRUE;
  for (i = 0; i < ARRAY_SIZE(xpdd->evtchn_pending_pvt[pcpu]); i++)
  {
    if (xpdd->evtchn_pending_pvt[pcpu][i])
    {
      *last_interrupt = FALSE;
      break;
    }
  }
  
  return (BOOLEAN)!!val;
}
#endif

//volatile ULONG in_inq = 0;

BOOLEAN
EvtChn_EvtInterruptIsr(WDFINTERRUPT interrupt, ULONG message_id)
{
/*
For HVM domains, Xen always triggers the event on CPU0. Because the
interrupt is delivered via the virtual PCI device it might get delivered
to CPU != 0, but we should always use vcpu_info[0]
*/
  int vcpu = 0;
  ULONG pcpu = KeGetCurrentProcessorNumber() & 0xff;
  vcpu_info_t *vcpu_info;
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(WdfInterruptGetDevice(interrupt));
  shared_info_t *shared_info_area = xpdd->shared_info_area;
  xen_ulong_t evt_words;
  unsigned long evt_word;
  unsigned long evt_bit;
  unsigned int port;
  ev_action_t *ev_action;
  BOOLEAN handled = FALSE;
  BOOLEAN deferred = FALSE;
  int i;

  UNREFERENCED_PARAMETER(message_id);

  if (xpdd->interrupts_masked) {
    FUNCTION_MSG("unhandled interrupt\n");
  }

  if (xpdd->hibernated) {
    FUNCTION_MSG("interrupt while hibernated\n");
  }

  for (i = 0; i < ARRAY_SIZE(xpdd->evtchn_pending_pvt[pcpu]); i++) {
    if (xpdd->evtchn_pending_pvt[pcpu][i]) {
      FUNCTION_MSG("Unacknowledged event word = %d, val = %p\n", i, xpdd->evtchn_pending_pvt[pcpu][i]);
      xpdd->evtchn_pending_pvt[pcpu][i] = 0;
    }
  }
  
  vcpu_info = &shared_info_area->vcpu_info[vcpu];

  vcpu_info->evtchn_upcall_pending = 0;

  if (xpdd->interrupts_masked)
  {
    return TRUE;
  }
  
  evt_words = (xen_ulong_t)xchg((volatile xen_long_t *)&vcpu_info->evtchn_pending_sel, 0);

  while (bit_scan_forward(&evt_word, evt_words))
  {
    evt_words &= ~(1 << evt_word);
    while (bit_scan_forward(&evt_bit, shared_info_area->evtchn_pending[evt_word] & ~shared_info_area->evtchn_mask[evt_word]))
    {
      synch_clear_bit(evt_bit, (volatile xen_long_t *)&shared_info_area->evtchn_pending[evt_word]);
      handled = TRUE;
      port = (evt_word << BITS_PER_LONG_SHIFT) + evt_bit;
      ev_action = &xpdd->ev_actions[port];
      ev_action->count++;
      switch (ev_action->type)
      {
      case EVT_ACTION_TYPE_NORMAL:
        //FUNCTION_MSG("EVT_ACTION_TYPE_NORMAL port = %d\n", port);
        ev_action->ServiceRoutine(ev_action->ServiceContext);
        break;
      case EVT_ACTION_TYPE_DPC:
        //FUNCTION_MSG("EVT_ACTION_TYPE_DPC port = %d\n", port);
        KeInsertQueueDpc(&ev_action->Dpc, NULL, NULL);
        break;
#if 0
      case EVT_ACTION_TYPE_SUSPEND:
        FUNCTION_MSG("EVT_ACTION_TYPE_SUSPEND\n");
        for (i = 0; i < NR_EVENTS; i++)
        {
          if (!(xpdd->ev_actions[i].flags & EVT_ACTION_FLAGS_NO_SUSPEND))
          {
            switch(xpdd->ev_actions[i].type)
            {
#if 0            
            case EVT_ACTION_TYPE_IRQ:
              {
                int suspend_bit = i & (BITS_PER_LONG - 1);
                int suspend_word = i >> BITS_PER_LONG_SHIFT;
                synch_set_bit(suspend_bit, (volatile xen_long_t *)&xpdd->evtchn_pending_pvt[pcpu][suspend_word]);
              }
              break;
#endif
            case EVT_ACTION_TYPE_NORMAL:
              if (xpdd->ev_actions[i].ServiceRoutine)
              {
                xpdd->ev_actions[i].ServiceRoutine(xpdd->ev_actions[i].ServiceContext);
              }
              break;
            case EVT_ACTION_TYPE_DPC:
              KeInsertQueueDpc(&xpdd->ev_actions[i].Dpc, NULL, NULL);
              break;
            }
          }
        }
        KeInsertQueueDpc(&ev_action->Dpc, NULL, NULL);
        deferred = TRUE;
        break;
#endif
      default:
        FUNCTION_MSG("Unhandled Event!!! port=%d\n", port);
        break;
      }
    }
  }

  return handled && !deferred;
}

NTSTATUS
EvtChn_EvtInterruptEnable(WDFINTERRUPT interrupt, WDFDEVICE device)
{
  NTSTATUS status = STATUS_SUCCESS;
  
  UNREFERENCED_PARAMETER(interrupt);
  UNREFERENCED_PARAMETER(device);

  FUNCTION_ENTER();
  FUNCTION_EXIT();

  return status;
}

NTSTATUS
EvtChn_EvtInterruptDisable(WDFINTERRUPT interrupt, WDFDEVICE device)
{
  NTSTATUS status = STATUS_SUCCESS;
  
  UNREFERENCED_PARAMETER(interrupt);
  UNREFERENCED_PARAMETER(device);

  FUNCTION_ENTER();
  FUNCTION_EXIT();

  return status;
}

NTSTATUS
EvtChn_Bind(PVOID Context, evtchn_port_t port, PXN_EVENT_CALLBACK ServiceRoutine, PVOID ServiceContext, ULONG flags)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  ev_action_t *action = &xpdd->ev_actions[port];

  FUNCTION_ENTER();
  
  if (InterlockedCompareExchange((volatile LONG *)&action->type, EVT_ACTION_TYPE_NEW, EVT_ACTION_TYPE_EMPTY) != EVT_ACTION_TYPE_EMPTY) {
    FUNCTION_MSG("Handler for port %d already registered\n", port);
    return STATUS_UNSUCCESSFUL;
  }

  xpdd->ev_actions[port].ServiceRoutine = ServiceRoutine;
  xpdd->ev_actions[port].ServiceContext = ServiceContext;
  xpdd->ev_actions[port].xpdd = xpdd;
  xpdd->ev_actions[port].flags = flags;
  KeMemoryBarrier();
  xpdd->ev_actions[port].type = EVT_ACTION_TYPE_NORMAL;

  EvtChn_Unmask(Context, port);

  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_BindDpc(PVOID Context, evtchn_port_t port, PXN_EVENT_CALLBACK ServiceRoutine, PVOID ServiceContext, ULONG flags) {
  PXENPCI_DEVICE_DATA xpdd = Context;
  ev_action_t *action = &xpdd->ev_actions[port];

  FUNCTION_ENTER();
  
  if (InterlockedCompareExchange((volatile LONG *)&action->type, EVT_ACTION_TYPE_NEW, EVT_ACTION_TYPE_EMPTY) != EVT_ACTION_TYPE_EMPTY) {
    FUNCTION_MSG(" Handler for port %d already registered\n", port);
    return STATUS_UNSUCCESSFUL;
  }

  xpdd->ev_actions[port].ServiceRoutine = ServiceRoutine;
  xpdd->ev_actions[port].ServiceContext = ServiceContext;
  xpdd->ev_actions[port].xpdd = xpdd;
  xpdd->ev_actions[port].flags = flags;
  KeMemoryBarrier(); // make sure that the new service routine is only called once the context is set up
  InterlockedExchange((volatile LONG *)&action->type, EVT_ACTION_TYPE_DPC);

  EvtChn_Unmask(Context, port);

  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}

#if 0
NTSTATUS
EvtChn_BindIrq(PVOID Context, evtchn_port_t port, ULONG vector, PCHAR description, ULONG flags)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  ev_action_t *action = &xpdd->ev_actions[port];

  FUNCTION_ENTER();
  
  if (InterlockedCompareExchange((volatile LONG *)&action->type, EVT_ACTION_TYPE_NEW, EVT_ACTION_TYPE_EMPTY) != EVT_ACTION_TYPE_EMPTY)
  {
    FUNCTION_MSG("Handler for port %d already registered\n", port);
    return STATUS_UNSUCCESSFUL;
  }

  xpdd->ev_actions[port].vector = vector;
  xpdd->ev_actions[port].xpdd = xpdd;
  KeMemoryBarrier();
  xpdd->ev_actions[port].type = EVT_ACTION_TYPE_IRQ;
  RtlStringCbCopyA(xpdd->ev_actions[port].description, 128, description);
  xpdd->ev_actions[port].flags = flags;
  
  EvtChn_Unmask(Context, port);

  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}
#endif

NTSTATUS
EvtChn_Unbind(PVOID Context, evtchn_port_t port) {
  PXENPCI_DEVICE_DATA xpdd = Context;
  ev_action_t *action = &xpdd->ev_actions[port];
  int old_type;
  
  EvtChn_Mask(Context, port);
  old_type = InterlockedExchange((volatile LONG *)&action->type, EVT_ACTION_TYPE_EMPTY);
  
  if (old_type == EVT_ACTION_TYPE_DPC) { // || old_type == EVT_ACTION_TYPE_SUSPEND) {
    KeRemoveQueueDpc(&xpdd->ev_actions[port].Dpc);
#if (NTDDI_VERSION >= NTDDI_WINXP)
    KeFlushQueuedDpcs();
#endif
  }
  
  KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
  xpdd->ev_actions[port].ServiceRoutine = NULL;
  xpdd->ev_actions[port].ServiceContext = NULL;

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Mask(PVOID Context, evtchn_port_t port) {
  PXENPCI_DEVICE_DATA xpdd = Context;

  synch_set_bit(port & (BITS_PER_LONG - 1),
    (volatile xen_long_t *)&xpdd->shared_info_area->evtchn_mask[port >> BITS_PER_LONG_SHIFT]);
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Unmask(PVOID context, evtchn_port_t port) {
  PXENPCI_DEVICE_DATA xpdd = context;

  synch_clear_bit(port & (BITS_PER_LONG - 1),
    (volatile xen_long_t *)&xpdd->shared_info_area->evtchn_mask[port >> BITS_PER_LONG_SHIFT]);
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Notify(PVOID context, evtchn_port_t port) {
  struct evtchn_send send;

  UNREFERENCED_PARAMETER(context);
  send.port = port;
  (void)HYPERVISOR_event_channel_op(EVTCHNOP_send, &send);
  return STATUS_SUCCESS;
}

evtchn_port_t
EvtChn_AllocIpi(PVOID context, ULONG vcpu) {
  evtchn_bind_ipi_t op;
  
  UNREFERENCED_PARAMETER(context);
  FUNCTION_ENTER();
  op.vcpu = vcpu;
  op.port = 0;
  HYPERVISOR_event_channel_op(EVTCHNOP_bind_ipi, &op);
  FUNCTION_EXIT();
  return op.port;
}

evtchn_port_t
EvtChn_AllocUnbound(PVOID context, domid_t Domain) {
  evtchn_alloc_unbound_t op;

  UNREFERENCED_PARAMETER(context);
  op.dom = DOMID_SELF;
  op.remote_dom = Domain;
  HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound, &op);
  return op.port;
}

VOID
EvtChn_Close(PVOID context, evtchn_port_t port) {
  evtchn_close_t op;

  UNREFERENCED_PARAMETER(context);
  op.port = port;
  HYPERVISOR_event_channel_op(EVTCHNOP_close, &op);
  return;
}

#if 0
VOID
EvtChn_PdoEventChannelDpc(PVOID context)
{
  PXENPCI_DEVICE_DATA xpdd = context;
  
  FUNCTION_ENTER();
  KeSetEvent(&xpdd->pdo_suspend_event, IO_NO_INCREMENT, FALSE);
  FUNCTION_EXIT();
}
#endif

NTSTATUS
EvtChn_Init(PXENPCI_DEVICE_DATA xpdd)
{
  ULONGLONG result;
  ev_action_t *action;
  int i;

  FUNCTION_ENTER();

  for (i = 0; i < NR_EVENTS; i++)
  {
    EvtChn_Mask(xpdd, i);
    action = &xpdd->ev_actions[i];
    action->type = EVT_ACTION_TYPE_EMPTY;
    //action->port = -1;
    action->count = 0;
    KeInitializeDpc(&action->Dpc, EvtChn_DpcBounce, action);
  }

  for (i = 0; i < 8; i++) {
    xpdd->shared_info_area->evtchn_pending[i] = 0;
  }

  for (i = 0; i < MAX_VIRT_CPUS; i++) {
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_pending = 0;
    xpdd->shared_info_area->vcpu_info[i].evtchn_pending_sel = 0;
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 1; /* apparantly this doesn't do anything */
  }

  KeMemoryBarrier();

  result = hvm_set_parameter(HVM_PARAM_CALLBACK_IRQ, xpdd->irq_number);
  FUNCTION_MSG("hvm_set_parameter(HVM_PARAM_CALLBACK_IRQ, %d) = %d\n", xpdd->irq_number, (ULONG)result);

  for (i = 0; i < MAX_VIRT_CPUS; i++)
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 0;  
  xpdd->interrupts_masked = FALSE;
  KeMemoryBarrier();

#if 0
  KeInitializeEvent(&xpdd->pdo_suspend_event, SynchronizationEvent, FALSE);
  xpdd->pdo_event_channel = EvtChn_AllocIpi(xpdd, 0);
  EvtChn_BindDpc(xpdd, xpdd->pdo_event_channel, EvtChn_PdoEventChannelDpc, xpdd, EVT_ACTION_FLAGS_DEFAULT);
  xpdd->ev_actions[xpdd->pdo_event_channel].type = EVT_ACTION_TYPE_SUSPEND; /* override dpc type */
  FUNCTION_MSG("pdo_event_channel = %d\n", xpdd->pdo_event_channel);
#endif  

  FUNCTION_EXIT();
  
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Suspend(PXENPCI_DEVICE_DATA xpdd)
{
  int i;

  xpdd->interrupts_masked = TRUE;
  for (i = 0; i < MAX_VIRT_CPUS; i++)
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 1;
  KeMemoryBarrier();
  hvm_set_parameter(HVM_PARAM_CALLBACK_IRQ, 0);

  for (i = 0; i < NR_EVENTS; i++) {
    if (xpdd->ev_actions[i].type == EVT_ACTION_TYPE_DPC) {
      KeRemoveQueueDpc(&xpdd->ev_actions[i].Dpc);
    }
  }
#if (NTDDI_VERSION >= NTDDI_WINXP)
  KeFlushQueuedDpcs();
#endif
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Resume(PXENPCI_DEVICE_DATA xpdd)
{
  return EvtChn_Init(xpdd);
}