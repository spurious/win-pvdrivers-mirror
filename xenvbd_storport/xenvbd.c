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

#define INITGUID

#include "xenvbd.h"

#pragma warning(disable: 4127)

/* Not really necessary but keeps PREfast happy */
DRIVER_INITIALIZE DriverEntry;
static IO_WORKITEM_ROUTINE XenVbd_DisconnectWorkItem;

static VOID XenVbd_HandleEventDpc(PSTOR_DPC dpc, PVOID DeviceExtension, PVOID arg1, PVOID arg2);
static VOID XenVbd_HandleEventDIRQL(PVOID DeviceExtension);
static VOID XenVbd_ProcessSrbList(PXENVBD_DEVICE_DATA xvdd);
static VOID XenVbd_DeviceCallback(PVOID context, ULONG callback_type, PVOID value);
static VOID XenVbd_StopRing(PXENVBD_DEVICE_DATA xvdd, BOOLEAN suspend);
static VOID XenVbd_StartRing(PXENVBD_DEVICE_DATA xvdd, BOOLEAN suspend);
static VOID XenVbd_CompleteDisconnect(PXENVBD_DEVICE_DATA xvdd);

//static BOOLEAN XenVbd_HwSxxxResetBus(PVOID DeviceExtension, ULONG PathId);

#define SxxxPortNotification(...) StorPortNotification(__VA_ARGS__)

static BOOLEAN dump_mode = FALSE;
#define DUMP_MODE_ERROR_LIMIT 64
static ULONG dump_mode_errors = 0;

#include "..\xenvbd_common\common_miniport.h"
#include "..\xenvbd_common\common_xen.h"

static VOID
XenVbd_StopRing(PXENVBD_DEVICE_DATA xvdd, BOOLEAN suspend) {
  NTSTATUS status;
  STOR_LOCK_HANDLE lock_handle;

  UNREFERENCED_PARAMETER(suspend);

  StorPortAcquireSpinLock(xvdd, StartIoLock, NULL, &lock_handle);
  xvdd->device_state = DEVICE_STATE_DISCONNECTING;
  if (xvdd->shadow_free == SHADOW_ENTRIES) {
    FUNCTION_MSG("Ring already empty\n");
    /* nothing on the ring - okay to disconnect now */
    StorPortReleaseSpinLock(xvdd, &lock_handle);
    status = XnWriteInt32(xvdd->handle, XN_BASE_FRONTEND, "state", XenbusStateClosing);
  } else {
    FUNCTION_MSG("Ring not empty - shadow_free = %d\n", xvdd->shadow_free);
    /* ring is busy. workitem will set XenbusStateClosing when its empty */
    StorPortReleaseSpinLock(xvdd, &lock_handle);
  }
}

static VOID
XenVbd_StartRing(PXENVBD_DEVICE_DATA xvdd, BOOLEAN suspend) {
  STOR_LOCK_HANDLE lock_handle;

  UNREFERENCED_PARAMETER(suspend);

  StorPortAcquireSpinLock(xvdd, StartIoLock, NULL, &lock_handle);
  XenVbd_ProcessSrbList(xvdd);
  StorPortReleaseSpinLock(xvdd, &lock_handle);
}

static VOID
XenVbd_DisconnectWorkItem(PDEVICE_OBJECT device_object, PVOID context) {
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)context;
  ULONG status;
  
  UNREFERENCED_PARAMETER(device_object);
  FUNCTION_ENTER();
  status = XnWriteInt32(xvdd->handle, XN_BASE_FRONTEND, "state", XenbusStateClosing);
  FUNCTION_EXIT();
}

static VOID
XenVbd_CompleteDisconnect(PXENVBD_DEVICE_DATA xvdd) {
    IoQueueWorkItem(xvdd->disconnect_workitem, XenVbd_DisconnectWorkItem, DelayedWorkQueue, xvdd);
}

/* called in non-dump mode */
static ULONG
XenVbd_VirtualHwStorFindAdapter(PVOID DeviceExtension, PVOID HwContext, PVOID BusInformation, PVOID LowerDevice, PCHAR ArgumentString, PPORT_CONFIGURATION_INFORMATION ConfigInfo, PBOOLEAN Again)
{
  NTSTATUS status;
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)DeviceExtension;

  //UNREFERENCED_PARAMETER(HwContext);
  UNREFERENCED_PARAMETER(BusInformation);
  UNREFERENCED_PARAMETER(LowerDevice);
  UNREFERENCED_PARAMETER(ArgumentString);

  FUNCTION_ENTER(); 
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  KdPrint((__DRIVER_NAME "     xvdd = %p\n", xvdd));

  if (XnGetVersion() != 1) {
    FUNCTION_MSG("Wrong XnGetVersion\n");
    FUNCTION_EXIT();
    return SP_RETURN_BAD_CONFIG;
  }

  RtlZeroMemory(xvdd, sizeof(XENVBD_DEVICE_DATA));
  InitializeListHead(&xvdd->srb_list);
  KeInitializeEvent(&xvdd->device_state_event, SynchronizationEvent, FALSE);
  KeInitializeEvent(&xvdd->backend_event, SynchronizationEvent, FALSE);
  xvdd->pdo = (PDEVICE_OBJECT)HwContext; // TODO: maybe should get PDO from FDO below? HwContext isn't really documented
  xvdd->fdo = (PDEVICE_OBJECT)BusInformation;
  xvdd->disconnect_workitem = IoAllocateWorkItem(xvdd->fdo);
  xvdd->aligned_buffer_in_use = FALSE;
  /* align the buffer to PAGE_SIZE */
  xvdd->aligned_buffer = (PVOID)((ULONG_PTR)((PUCHAR)xvdd->aligned_buffer_data + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
  KdPrint((__DRIVER_NAME "     aligned_buffer_data = %p\n", xvdd->aligned_buffer_data));
  KdPrint((__DRIVER_NAME "     aligned_buffer = %p\n", xvdd->aligned_buffer));

  ConfigInfo->MaximumTransferLength = 4 * 1024 * 1024; //BLKIF_MAX_SEGMENTS_PER_REQUEST * PAGE_SIZE;
  ConfigInfo->NumberOfPhysicalBreaks = ConfigInfo->MaximumTransferLength >> PAGE_SHIFT; //BLKIF_MAX_SEGMENTS_PER_REQUEST - 1;
  FUNCTION_MSG("ConfigInfo->MaximumTransferLength = %d\n", ConfigInfo->MaximumTransferLength);
  FUNCTION_MSG("ConfigInfo->NumberOfPhysicalBreaks = %d\n", ConfigInfo->NumberOfPhysicalBreaks);
  if (!dump_mode) {
    ConfigInfo->VirtualDevice = TRUE;
    xvdd->aligned_buffer_size = BLKIF_MAX_SEGMENTS_PER_REQUEST * PAGE_SIZE;
  } else {
    ConfigInfo->VirtualDevice = FALSE;
    xvdd->aligned_buffer_size = DUMP_MODE_UNALIGNED_PAGES * PAGE_SIZE;
  }
  status = XenVbd_Connect(DeviceExtension, FALSE);
 
  FUNCTION_MSG("ConfigInfo->VirtualDevice = %d\n", ConfigInfo->VirtualDevice);
  ConfigInfo->ScatterGather = TRUE;
  ConfigInfo->Master = TRUE;
  ConfigInfo->CachesData = FALSE;
  ConfigInfo->MapBuffers = STOR_MAP_ALL_BUFFERS;
  FUNCTION_MSG("ConfigInfo->NeedPhysicalAddresses = %d\n", ConfigInfo->NeedPhysicalAddresses);
  ConfigInfo->SynchronizationModel = StorSynchronizeFullDuplex;
  ConfigInfo->AlignmentMask = 0;
  ConfigInfo->NumberOfBuses = 1;
  ConfigInfo->InitiatorBusId[0] = 1;
  ConfigInfo->MaximumNumberOfLogicalUnits = 1;
  ConfigInfo->MaximumNumberOfTargets = 2;
  if (ConfigInfo->Dma64BitAddresses == SCSI_DMA64_SYSTEM_SUPPORTED) {
    ConfigInfo->Dma64BitAddresses = SCSI_DMA64_MINIPORT_SUPPORTED;
    FUNCTION_MSG("Dma64BitAddresses supported\n");
  } else {
    FUNCTION_MSG("Dma64BitAddresses not supported\n");
  }
  *Again = FALSE;

  FUNCTION_EXIT();

  return SP_RETURN_FOUND;
}

/* called in dump mode */
static ULONG
XenVbd_HwStorFindAdapter(PVOID DeviceExtension, PVOID HwContext, PVOID BusInformation, PCHAR ArgumentString, PPORT_CONFIGURATION_INFORMATION ConfigInfo, PBOOLEAN Again)
{
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)DeviceExtension;

  UNREFERENCED_PARAMETER(HwContext);
  UNREFERENCED_PARAMETER(BusInformation);
  UNREFERENCED_PARAMETER(ArgumentString);
  
  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  KdPrint((__DRIVER_NAME "     xvdd = %p\n", xvdd));

  memcpy(xvdd, ConfigInfo->Reserved, FIELD_OFFSET(XENVBD_DEVICE_DATA, aligned_buffer_data));
  InitializeListHead(&xvdd->srb_list);
  xvdd->aligned_buffer_in_use = FALSE;
  /* align the buffer to PAGE_SIZE */
  xvdd->aligned_buffer = (PVOID)((ULONG_PTR)((PUCHAR)xvdd->aligned_buffer_data + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
  KdPrint((__DRIVER_NAME "     aligned_buffer_data = %p\n", xvdd->aligned_buffer_data));
  KdPrint((__DRIVER_NAME "     aligned_buffer = %p\n", xvdd->aligned_buffer));

  ConfigInfo->MaximumTransferLength = 4 * 1024 * 1024;
  ConfigInfo->NumberOfPhysicalBreaks = ConfigInfo->MaximumTransferLength >> PAGE_SHIFT;
  FUNCTION_MSG("ConfigInfo->MaximumTransferLength = %d\n", ConfigInfo->MaximumTransferLength);
  FUNCTION_MSG("ConfigInfo->NumberOfPhysicalBreaks = %d\n", ConfigInfo->NumberOfPhysicalBreaks);
  ConfigInfo->VirtualDevice = FALSE;
  xvdd->aligned_buffer_size = DUMP_MODE_UNALIGNED_PAGES * PAGE_SIZE;
  ConfigInfo->ScatterGather = TRUE;
  ConfigInfo->Master = TRUE;
  ConfigInfo->CachesData = FALSE;
  ConfigInfo->MapBuffers = STOR_MAP_ALL_BUFFERS;
  FUNCTION_MSG("ConfigInfo->NeedPhysicalAddresses = %d\n", ConfigInfo->NeedPhysicalAddresses);
  ConfigInfo->SynchronizationModel = StorSynchronizeFullDuplex;
  ConfigInfo->AlignmentMask = 0;
  ConfigInfo->NumberOfBuses = 1;
  ConfigInfo->InitiatorBusId[0] = 1;
  ConfigInfo->MaximumNumberOfLogicalUnits = 1;
  ConfigInfo->MaximumNumberOfTargets = 2;
  if (ConfigInfo->Dma64BitAddresses == SCSI_DMA64_SYSTEM_SUPPORTED) {
    ConfigInfo->Dma64BitAddresses = SCSI_DMA64_MINIPORT_SUPPORTED;
    FUNCTION_MSG("Dma64BitAddresses supported\n");
  } else {
    FUNCTION_MSG("Dma64BitAddresses not supported\n");
  }
  *Again = FALSE;

  FUNCTION_EXIT();

  return SP_RETURN_FOUND;
}

/* Called at PASSIVE_LEVEL for non-dump mode */
static BOOLEAN
XenVbd_HwStorInitialize(PVOID DeviceExtension)
{
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)DeviceExtension;
  ULONG i;
  
  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  KdPrint((__DRIVER_NAME "     dump_mode = %d\n", dump_mode));
  
  xvdd->shadow_free = 0;
  memset(xvdd->shadows, 0, sizeof(blkif_shadow_t) * SHADOW_ENTRIES);
  for (i = 0; i < SHADOW_ENTRIES; i++) {
    xvdd->shadows[i].req.id = i;
    /* make sure leftover real requests's are never confused with dump mode requests */
    if (dump_mode)
      xvdd->shadows[i].req.id |= SHADOW_ID_DUMP_FLAG;
    put_shadow_on_freelist(xvdd, &xvdd->shadows[i]);
  }

  if (!dump_mode) {
    StorPortInitializeDpc(DeviceExtension, &xvdd->dpc, XenVbd_HandleEventDpc);
  } else {
    xvdd->grant_tag = (ULONG)'DUMP';
  }
  
  FUNCTION_EXIT();

  return TRUE;
}

#if 0
/* called with StartIo lock held */
static VOID
XenVbd_HandleEvent(PVOID DeviceExtension)
{
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)DeviceExtension;
  PSCSI_REQUEST_BLOCK srb;
  RING_IDX i, rp;
  ULONG j;
  blkif_response_t *rep;
  //int block_count;
  int more_to_do = TRUE;
  blkif_shadow_t *shadow;
  srb_list_entry_t *srb_entry;

  //if (dump_mode) FUNCTION_ENTER();

  while (more_to_do)
  {
    rp = xvdd->ring.sring->rsp_prod;
    KeMemoryBarrier();
    for (i = xvdd->ring.rsp_cons; i != rp; i++)
    {
      rep = XenVbd_GetResponse(xvdd, i);
      shadow = &xvdd->shadows[rep->id & SHADOW_ID_ID_MASK];
      if (shadow->reset)
      {
        KdPrint((__DRIVER_NAME "     discarding reset shadow\n"));
        for (j = 0; j < shadow->req.nr_segments; j++)
        {
          XnEndAccess(xvdd->handle,
            shadow->req.seg[j].gref, FALSE, xvdd->grant_tag);
        }
      }
      else if (dump_mode && !(rep->id & SHADOW_ID_DUMP_FLAG))
      {
        KdPrint((__DRIVER_NAME "     discarding stale (non-dump-mode) shadow\n"));
      }
      else
      {
        srb = shadow->srb;
        NT_ASSERT(srb);
        srb_entry = srb->SrbExtension;
        NT_ASSERT(srb_entry);
        /* a few errors occur in dump mode because Xen refuses to allow us to map pages we are using for other stuff. Just ignore them */
        if (rep->status == BLKIF_RSP_OKAY || (dump_mode &&  dump_mode_errors++ < DUMP_MODE_ERROR_LIMIT))
          srb->SrbStatus = SRB_STATUS_SUCCESS;
        else
        {
          KdPrint((__DRIVER_NAME "     Xen Operation returned error\n"));
          if (decode_cdb_is_read(srb))
            KdPrint((__DRIVER_NAME "     Operation = Read\n"));
          else
            KdPrint((__DRIVER_NAME "     Operation = Write\n"));
          srb_entry->error = TRUE;
        }
        if (shadow->aligned_buffer_in_use)
        {
          NT_ASSERT(xvdd->aligned_buffer_in_use);
          xvdd->aligned_buffer_in_use = FALSE;
          if (srb->SrbStatus == SRB_STATUS_SUCCESS && decode_cdb_is_read(srb))
            memcpy((PUCHAR)shadow->system_address, xvdd->aligned_buffer, shadow->length);
        }
        for (j = 0; j < shadow->req.nr_segments; j++)
        {
          XnEndAccess(xvdd->handle, shadow->req.seg[j].gref, FALSE, xvdd->grant_tag);
        }
        srb_entry->outstanding_requests--;
        if (!srb_entry->outstanding_requests && srb_entry->offset == srb_entry->length)
        {
          if (srb_entry->error)
          {
            srb->SrbStatus = SRB_STATUS_ERROR;
            srb->ScsiStatus = 0x02;
            xvdd->last_sense_key = SCSI_SENSE_MEDIUM_ERROR;
            xvdd->last_additional_sense_code = SCSI_ADSENSE_NO_SENSE;
            XenVbd_MakeAutoSense(xvdd, srb);
          }        
          StorPortNotification(RequestComplete, xvdd, srb);
        }
      }
      put_shadow_on_freelist(xvdd, shadow);
    }

    xvdd->ring.rsp_cons = i;
    if (i != xvdd->ring.req_prod_pvt)
    {
      RING_FINAL_CHECK_FOR_RESPONSES(&xvdd->ring, more_to_do);
    }
    else
    {
      xvdd->ring.sring->rsp_event = i + 1;
      more_to_do = FALSE;
    }
  }

  if (dump_mode || xvdd->device_state == DEVICE_STATE_ACTIVE) {
    XenVbd_ProcessSrbList(xvdd);
  } else if (xvdd->shadow_free == SHADOW_ENTRIES) {
    FUNCTION_MSG("ring now empty - scheduling workitem for disconnect\n");
    IoQueueWorkItem(xvdd->disconnect_workitem, XenVbd_DisconnectWorkItem, DelayedWorkQueue, xvdd);
  }
  //if (dump_mode) FUNCTION_EXIT();
  return;
}
#endif

static VOID
XenVbd_HandleEventDpc(PSTOR_DPC dpc, PVOID DeviceExtension, PVOID arg1, PVOID arg2) {
  STOR_LOCK_HANDLE lock_handle;
  UNREFERENCED_PARAMETER(dpc);
  UNREFERENCED_PARAMETER(arg1);
  UNREFERENCED_PARAMETER(arg2);
  
  StorPortAcquireSpinLock(DeviceExtension, StartIoLock, NULL, &lock_handle);
  XenVbd_HandleEvent(DeviceExtension);
  StorPortReleaseSpinLock(DeviceExtension, &lock_handle);
}

static VOID
XenVbd_HandleEventDIRQL(PVOID DeviceExtension) {
  PXENVBD_DEVICE_DATA xvdd = DeviceExtension;
  //if (dump_mode) FUNCTION_ENTER();
  StorPortIssueDpc(DeviceExtension, &xvdd->dpc, NULL, NULL);
  //if (dump_mode) FUNCTION_EXIT();
  return;
}

/* this is only used during hiber and dump */
static BOOLEAN
XenVbd_HwStorInterrupt(PVOID DeviceExtension)
{
  //FUNCTION_ENTER();
  XenVbd_HandleEvent(DeviceExtension);
  //FUNCTION_EXIT();
  return TRUE;
}

#if 0
static BOOLEAN
XenVbd_HwSxxxResetBus(PVOID DeviceExtension, ULONG PathId)
{
  PXENVBD_DEVICE_DATA xvdd = DeviceExtension;
  //srb_list_entry_t *srb_entry;
  int i;
  /* need to make sure that each SRB is only reset once */
  LIST_ENTRY srb_reset_list;
  PLIST_ENTRY list_entry;
  //STOR_LOCK_HANDLE lock_handle;

  UNREFERENCED_PARAMETER(PathId);

  FUNCTION_ENTER();
  
  if (dump_mode) {
    FUNCTION_MSG("dump mode - doing nothing\n");
    FUNCTION_EXIT();
    return TRUE;
  }

  /* It appears that the StartIo spinlock is already held at this point */

  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));

  xvdd->aligned_buffer_in_use = FALSE;
  
  InitializeListHead(&srb_reset_list);
  
  while((list_entry = RemoveHeadList(&xvdd->srb_list)) != &xvdd->srb_list) {
    #if DBG
    srb_list_entry_t *srb_entry = CONTAINING_RECORD(list_entry, srb_list_entry_t, list_entry);
    KdPrint((__DRIVER_NAME "     adding queued SRB %p to reset list\n", srb_entry->srb));
    #endif
    InsertTailList(&srb_reset_list, list_entry);
  }
  
  for (i = 0; i < MAX_SHADOW_ENTRIES; i++) {
    if (xvdd->shadows[i].srb) {
      srb_list_entry_t *srb_entry = xvdd->shadows[i].srb->SrbExtension;
      for (list_entry = srb_reset_list.Flink; list_entry != &srb_reset_list; list_entry = list_entry->Flink) {
        if (list_entry == &srb_entry->list_entry)
          break;
      }
      if (list_entry == &srb_reset_list) {
        KdPrint((__DRIVER_NAME "     adding in-flight SRB %p to reset list\n", srb_entry->srb));
        InsertTailList(&srb_reset_list, &srb_entry->list_entry);
      }
      /* set reset here so that the interrupt won't do anything with the srb but will dispose of the shadow entry correctly */
      xvdd->shadows[i].reset = TRUE;
      xvdd->shadows[i].srb = NULL;
      xvdd->shadows[i].aligned_buffer_in_use = FALSE;
    }
  }

  while((list_entry = RemoveHeadList(&srb_reset_list)) != &srb_reset_list) {
    srb_list_entry_t *srb_entry = CONTAINING_RECORD(list_entry, srb_list_entry_t, list_entry);
    srb_entry->srb->SrbStatus = SRB_STATUS_BUS_RESET;
    KdPrint((__DRIVER_NAME "     completing SRB %p with status SRB_STATUS_BUS_RESET\n", srb_entry->srb));
    StorPortNotification(RequestComplete, xvdd, srb_entry->srb);
  }

  /* send a notify to Dom0 just in case it was missed for some reason (which should _never_ happen normally but could in dump mode) */
  XnNotify(xvdd->handle, xvdd->event_channel);

  StorPortNotification(NextRequest, DeviceExtension);
  FUNCTION_EXIT();

  return TRUE;
}
#endif

static BOOLEAN
XenVbd_HwStorResetBus(PVOID DeviceExtension, ULONG PathId)
{
  PXENVBD_DEVICE_DATA xvdd = DeviceExtension;
  return XenVbd_ResetBus(xvdd, PathId);
}

static BOOLEAN
XenVbd_HwStorStartIo(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK srb)
{
  PXENVBD_DEVICE_DATA xvdd = DeviceExtension;
  STOR_LOCK_HANDLE lock_handle;

  //if (dump_mode) FUNCTION_ENTER();
  //if (dump_mode) KdPrint((__DRIVER_NAME "     srb = %p\n", srb));
  
  StorPortAcquireSpinLock(DeviceExtension, StartIoLock, NULL, &lock_handle);
  
  if (xvdd->device_state == DEVICE_STATE_INACTIVE) {
    FUNCTION_MSG("HwStorStartIo Inactive Device (in StartIo)\n");
    srb->SrbStatus = SRB_STATUS_NO_DEVICE;
    StorPortNotification(RequestComplete, DeviceExtension, srb);
    StorPortReleaseSpinLock (DeviceExtension, &lock_handle);
    return TRUE;
  }

  if (srb->PathId != 0 || srb->TargetId != 0 || srb->Lun != 0)
  {
    FUNCTION_MSG("HwStorStartIo (Out of bounds - PathId = %d, TargetId = %d, Lun = %d)\n", srb->PathId, srb->TargetId, srb->Lun);
    srb->SrbStatus = SRB_STATUS_NO_DEVICE;
    StorPortNotification(RequestComplete, DeviceExtension, srb);
    StorPortReleaseSpinLock (DeviceExtension, &lock_handle);
    return TRUE;
  }

  XenVbd_PutSrbOnList(xvdd, srb);
  XenVbd_ProcessSrbList(xvdd);
  StorPortReleaseSpinLock (DeviceExtension, &lock_handle);
  //if (dump_mode) FUNCTION_EXIT();
  return TRUE;
}

static SCSI_ADAPTER_CONTROL_STATUS
XenVbd_HwStorAdapterControl(PVOID DeviceExtension, SCSI_ADAPTER_CONTROL_TYPE ControlType, PVOID Parameters)
{
  PXENVBD_DEVICE_DATA xvdd = DeviceExtension;
  SCSI_ADAPTER_CONTROL_STATUS Status = ScsiAdapterControlSuccess;
  PSCSI_SUPPORTED_CONTROL_TYPE_LIST SupportedControlTypeList;
  //KIRQL OldIrql;

  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  KdPrint((__DRIVER_NAME "     xvdd = %p\n", xvdd));

  switch (ControlType)
  {
  case ScsiQuerySupportedControlTypes:
    SupportedControlTypeList = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
    KdPrint((__DRIVER_NAME "     ScsiQuerySupportedControlTypes (Max = %d)\n", SupportedControlTypeList->MaxControlType));
    SupportedControlTypeList->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;
    SupportedControlTypeList->SupportedTypeList[ScsiStopAdapter] = TRUE;
    SupportedControlTypeList->SupportedTypeList[ScsiRestartAdapter] = TRUE;
    break;
  case ScsiStopAdapter:
    KdPrint((__DRIVER_NAME "     ScsiStopAdapter\n"));
    if (xvdd->device_state == DEVICE_STATE_INACTIVE)
    {
      KdPrint((__DRIVER_NAME "     inactive - nothing to do\n"));
      break;
    }
    NT_ASSERT(IsListEmpty(&xvdd->srb_list));
    NT_ASSERT(xvdd->shadow_free == SHADOW_ENTRIES);
    break;
  case ScsiRestartAdapter:
    KdPrint((__DRIVER_NAME "     ScsiRestartAdapter\n"));
    if (xvdd->device_state == DEVICE_STATE_INACTIVE)
    {
      KdPrint((__DRIVER_NAME "     inactive - nothing to do\n"));
      break;
    }
    /* increase the tag every time we stop/start to track where the gref's came from */
    xvdd->grant_tag++;
    break;
  case ScsiSetBootConfig:
    KdPrint((__DRIVER_NAME "     ScsiSetBootConfig\n"));
    break;
  case ScsiSetRunningConfig:
    KdPrint((__DRIVER_NAME "     ScsiSetRunningConfig\n"));
    break;
  default:
    KdPrint((__DRIVER_NAME "     UNKNOWN\n"));
    break;
  }

  FUNCTION_EXIT();
  
  return Status;
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
  ULONG status;
  VIRTUAL_HW_INITIALIZATION_DATA VHwInitializationData;
  HW_INITIALIZATION_DATA HwInitializationData;
  
  if (!RegistryPath) {
    dump_mode = TRUE;
    // what if hibernate and not dump?
    XnDumpModeHookDebugPrint();
  }

  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  KdPrint((__DRIVER_NAME "     DriverObject = %p, RegistryPath = %p\n", DriverObject, RegistryPath));

  /* RegistryPath == NULL when we are invoked as a crash dump driver */
  
  if (!dump_mode) {
    RtlZeroMemory(&VHwInitializationData, sizeof(VIRTUAL_HW_INITIALIZATION_DATA));
    VHwInitializationData.HwInitializationDataSize = sizeof(VIRTUAL_HW_INITIALIZATION_DATA);
    VHwInitializationData.AdapterInterfaceType = Internal; //PNPBus; /* maybe should be internal? */
    VHwInitializationData.DeviceExtensionSize = FIELD_OFFSET(XENVBD_DEVICE_DATA, aligned_buffer_data) + UNALIGNED_BUFFER_DATA_SIZE;
    VHwInitializationData.SpecificLuExtensionSize = 0;
    VHwInitializationData.SrbExtensionSize = sizeof(srb_list_entry_t);
    VHwInitializationData.NumberOfAccessRanges = 1;
    VHwInitializationData.MapBuffers = STOR_MAP_ALL_BUFFERS;
    //VHwInitializationData.NeedPhysicalAddresses  = TRUE;
    VHwInitializationData.TaggedQueuing = TRUE;
    VHwInitializationData.AutoRequestSense = TRUE;
    VHwInitializationData.MultipleRequestPerLu = TRUE;
    VHwInitializationData.ReceiveEvent = TRUE;
    VHwInitializationData.PortVersionFlags = 0;
    VHwInitializationData.HwInitialize = XenVbd_HwStorInitialize;
    VHwInitializationData.HwStartIo = XenVbd_HwStorStartIo;
    VHwInitializationData.HwFindAdapter = XenVbd_VirtualHwStorFindAdapter;
    VHwInitializationData.HwResetBus = XenVbd_HwStorResetBus;
    VHwInitializationData.HwAdapterControl = XenVbd_HwStorAdapterControl;
    status = StorPortInitialize(DriverObject, RegistryPath, (PHW_INITIALIZATION_DATA)&VHwInitializationData, NULL);
  } else {
    RtlZeroMemory(&HwInitializationData, sizeof(HW_INITIALIZATION_DATA));
    HwInitializationData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
    HwInitializationData.AdapterInterfaceType = Internal; //PNPBus; /* not Internal */
    HwInitializationData.DeviceExtensionSize = FIELD_OFFSET(XENVBD_DEVICE_DATA, aligned_buffer_data) + UNALIGNED_BUFFER_DATA_SIZE_DUMP_MODE;
    HwInitializationData.SrbExtensionSize = sizeof(srb_list_entry_t);
    HwInitializationData.NumberOfAccessRanges = 1;
    HwInitializationData.MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;
    HwInitializationData.NeedPhysicalAddresses  = TRUE;
    HwInitializationData.TaggedQueuing = FALSE;
    HwInitializationData.AutoRequestSense = TRUE;
    HwInitializationData.MultipleRequestPerLu = FALSE;
    HwInitializationData.ReceiveEvent = TRUE;
    HwInitializationData.HwInitialize = XenVbd_HwStorInitialize;
    HwInitializationData.HwStartIo = XenVbd_HwStorStartIo;
    HwInitializationData.HwFindAdapter = XenVbd_HwStorFindAdapter;
    HwInitializationData.HwResetBus = XenVbd_HwStorResetBus;
    HwInitializationData.HwAdapterControl = XenVbd_HwStorAdapterControl;
    HwInitializationData.HwInterrupt = XenVbd_HwStorInterrupt;
    status = StorPortInitialize(DriverObject, RegistryPath, &HwInitializationData, NULL);
  }
  
  if(!NT_SUCCESS(status)) {
    FUNCTION_MSG("ScsiPortInitialize failed with status 0x%08x\n", status);
  }

  FUNCTION_EXIT();

  return status;
}