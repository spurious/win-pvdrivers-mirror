/*
PV Drivers for Windows Xen HVM Domains
Copyright (C) 2007 James Harper

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "xenpci.h"

#if defined(_WIN32)
  #define xchg(p1, p2) _InterlockedExchange(p1, p2)
  #define synch_clear_bit(p1, p2) _interlockedbittestandreset(p2, p1)
  #define synch_set_bit(p1, p2) _interlockedbittestandset(p2, p1)
  #define bit_scan_forward(p1, p2) _BitScanForward(p1, p2)
#else
  #define xchg(p1, p2) _InterlockedExchange64(p1, p2)
  #define synch_clear_bit(p1, p2) _interlockedbittestandreset64(p2, p1)
  #define synch_set_bit(p1, p2) _interlockedbittestandset64(p2, p1)
  #define bit_scan_forward(p1, p2) _BitScanForward64(p1, p2)
#endif

static VOID
EvtChn_DpcBounce(PRKDPC Dpc, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2)
{
  ev_action_t *action = Context;

  UNREFERENCED_PARAMETER(Dpc);
  UNREFERENCED_PARAMETER(SystemArgument1);
  UNREFERENCED_PARAMETER(SystemArgument2);

  action->ServiceRoutine(NULL, action->ServiceContext);
}

static BOOLEAN
EvtChn_Interrupt(PKINTERRUPT Interrupt, PVOID Context)
{
  int cpu = KeGetCurrentProcessorNumber() & (MAX_VIRT_CPUS - 1);
  vcpu_info_t *vcpu_info;
  PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)Context;
  shared_info_t *shared_info_area = xpdd->shared_info_area;
  xen_ulong_t evt_words;
  unsigned long evt_word;
  unsigned long evt_bit;
  unsigned int port;
  ev_action_t *ev_action;

  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ " (cpu = %d)\n", cpu));

  UNREFERENCED_PARAMETER(Interrupt);

  vcpu_info = &shared_info_area->vcpu_info[cpu];

  vcpu_info->evtchn_upcall_pending = 0;

  evt_words = (xen_ulong_t)xchg((volatile xen_long_t *)&vcpu_info->evtchn_pending_sel, 0);
  
  while (bit_scan_forward(&evt_word, evt_words))
  {
    evt_words &= ~(1 << evt_word);
    while (bit_scan_forward(&evt_bit, shared_info_area->evtchn_pending[evt_word] & ~shared_info_area->evtchn_mask[evt_word]))
    {
      port = (evt_word << 5) + evt_bit;
      ev_action = &xpdd->ev_actions[port];
      if (ev_action->ServiceRoutine == NULL)
      {
        KdPrint((__DRIVER_NAME "     Unhandled Event!!!\n"));
      }
      else
      {
        if (ev_action->DpcFlag)
        {
          KdPrint((__DRIVER_NAME "     Scheduling Dpc\n"));
          KeInsertQueueDpc(&ev_action->Dpc, NULL, NULL);
        }
        else
        {
          ev_action->ServiceRoutine(NULL, ev_action->ServiceContext);
        }
      }
      synch_clear_bit(port, (volatile xen_long_t *)&shared_info_area->evtchn_pending[evt_word]);
    }
  }

  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));

  /* Need to return FALSE so we can fall through to the scsiport ISR. */
  return FALSE;
}

NTSTATUS
EvtChn_Bind(PVOID Context, evtchn_port_t Port, PKSERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  //KdPrint((__DRIVER_NAME " --> EvtChn_Bind (ServiceRoutine = %08X, ServiceContext = %08x)\n", ServiceRoutine, ServiceContext));

  if(xpdd->ev_actions[Port].ServiceRoutine != NULL)
  {
    xpdd->ev_actions[Port].ServiceRoutine = NULL;
    KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
    KdPrint((__DRIVER_NAME " Handler for port %d already registered, replacing\n", Port));
  }

  xpdd->ev_actions[Port].DpcFlag = FALSE;
  xpdd->ev_actions[Port].ServiceContext = ServiceContext;
  KeMemoryBarrier();
  xpdd->ev_actions[Port].ServiceRoutine = ServiceRoutine;

  EvtChn_Unmask(Context, Port);

  KdPrint((__DRIVER_NAME " <-- EvtChn_Bind\n"));

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_BindDpc(PVOID Context, evtchn_port_t Port, PKSERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  KdPrint((__DRIVER_NAME " --> EvtChn_BindDpc\n"));

  if(xpdd->ev_actions[Port].ServiceRoutine != NULL)
  {
    KdPrint((__DRIVER_NAME " Handler for port %d already registered, replacing\n", Port));
    xpdd->ev_actions[Port].ServiceRoutine = NULL;
    KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
  }

  xpdd->ev_actions[Port].ServiceContext = ServiceContext;
  xpdd->ev_actions[Port].DpcFlag = TRUE;

  KeInitializeDpc(&xpdd->ev_actions[Port].Dpc, EvtChn_DpcBounce, &xpdd->ev_actions[Port]);

  KeMemoryBarrier(); // make sure that the new service routine is only called once the context is set up
  xpdd->ev_actions[Port].ServiceRoutine = ServiceRoutine;

  EvtChn_Unmask(Context, Port);

  KdPrint((__DRIVER_NAME " <-- EvtChn_BindDpc\n"));

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Unbind(PVOID Context, evtchn_port_t Port)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  
  EvtChn_Mask(Context, Port);
  xpdd->ev_actions[Port].ServiceRoutine = NULL;
  KeMemoryBarrier();
  xpdd->ev_actions[Port].ServiceContext = NULL;

  if (xpdd->ev_actions[Port].DpcFlag)
    KeRemoveQueueDpc(&xpdd->ev_actions[Port].Dpc);
  
  //KdPrint((__DRIVER_NAME " <-- EvtChn_UnBind\n"));

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Mask(PXENPCI_DEVICE_DATA xpdd, evtchn_port_t Port)
{
  synch_set_bit(Port,
    (volatile xen_long_t *)&xpdd->shared_info_area->evtchn_mask[0]);
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Unmask(PXENPCI_DEVICE_DATA xpdd , evtchn_port_t Port)
{
  synch_clear_bit(Port,
    (volatile xen_long_t *)&xpdd->shared_info_area->evtchn_mask[0]);
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Notify(PXENPCI_DEVICE_DATA xpdd, evtchn_port_t Port)
{
  struct evtchn_send send;

  send.port = Port;
  (void)HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_send, &send);
  return STATUS_SUCCESS;
}

evtchn_port_t
EvtChn_AllocUnbound(PVOID Context, domid_t Domain)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  evtchn_alloc_unbound_t op;
  op.dom = DOMID_SELF;
  op.remote_dom = Domain;
  HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_alloc_unbound, &op);
  return op.port;
}

NTSTATUS
EvtChn_Init(PXENPCI_DEVICE_DATA xpdd)
{
  NTSTATUS status;
  int i;

  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));

  for (i = 0; i < NR_EVENTS; i++)
  {
    EvtChn_Mask(xpdd, i);
    xpdd->ev_actions[i].ServiceRoutine = NULL;
    xpdd->ev_actions[i].ServiceContext = NULL;
    xpdd->ev_actions[i].Count = 0;
  }

  for (i = 0; i < 8; i++)
  {
    xpdd->shared_info_area->evtchn_pending[i] = 0;
  }

  for (i = 0; i < MAX_VIRT_CPUS; i++)
  {
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_pending = 0;
    xpdd->shared_info_area->vcpu_info[i].evtchn_pending_sel = 0;
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 1;
  }

  status = IoConnectInterrupt(
    &xpdd->interrupt,
	EvtChn_Interrupt,
	xpdd,
	NULL,
	xpdd->irq_vector,
	xpdd->irq_level,
	xpdd->irq_level,
	LevelSensitive,
	TRUE, /* this is a bit of a hack to make xenvbd work */
	xpdd->irq_affinity,
	FALSE);
  
  if (!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME "     IoConnectInterrupt failed 0x%08x\n", status));
    return status;
  }

  hvm_set_parameter(xpdd, HVM_PARAM_CALLBACK_IRQ, xpdd->irq_number);

  for (i = 0; i < MAX_VIRT_CPUS; i++)
  {
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 0;
  }
  
  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));

  return status;
}

NTSTATUS
EvtChn_Shutdown(PXENPCI_DEVICE_DATA xpdd)
{
  UNREFERENCED_PARAMETER(xpdd);

  return STATUS_SUCCESS;
}
