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

static VOID XenVbd_ProcessSrbList(PXENVBD_DEVICE_DATA xvdd);
static BOOLEAN XenVbd_ResetBus(PXENVBD_DEVICE_DATA xvdd, ULONG PathId);
static VOID XenVbd_CompleteDisconnect(PXENVBD_DEVICE_DATA xvdd);

static BOOLEAN dump_mode = FALSE;
#define DUMP_MODE_ERROR_LIMIT 64
static ULONG dump_mode_errors = 0;

#define StorPortAcquireSpinLock(...) {}
#define StorPortReleaseSpinLock(...) {}

#define SxxxPortNotification(NotificationType, DeviceExtension, ...) \
{ \
  if (NotificationType == RequestComplete) ((PXENVBD_SCSIPORT_DATA)((PXENVBD_DEVICE_DATA)DeviceExtension)->xvsd)->outstanding--; \
  ScsiPortNotification(NotificationType, ((PXENVBD_DEVICE_DATA)DeviceExtension)->xvsd, __VA_ARGS__); \
}

#include "..\xenvbd_common\common_miniport.h"

/* called in non-dump mode */
static ULONG
XenVbd_HwScsiFindAdapter(PVOID DeviceExtension, PVOID HwContext, PVOID BusInformation, PCHAR ArgumentString, PPORT_CONFIGURATION_INFORMATION ConfigInfo, PBOOLEAN Again)
{
  //NTSTATUS status;
  PXENVBD_SCSIPORT_DATA xvsd = (PXENVBD_SCSIPORT_DATA)DeviceExtension;
  PXENVBD_DEVICE_DATA xvdd;
  PACCESS_RANGE access_range;

  UNREFERENCED_PARAMETER(HwContext);
  UNREFERENCED_PARAMETER(BusInformation);
  UNREFERENCED_PARAMETER(ArgumentString);

  FUNCTION_ENTER(); 
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  KdPrint((__DRIVER_NAME "     xvsd = %p\n", xvsd));

  if (ConfigInfo->NumberOfAccessRanges != 1) {
    FUNCTION_MSG("NumberOfAccessRanges wrong\n");
    FUNCTION_EXIT();
    return SP_RETURN_BAD_CONFIG;
  }
  if (XnGetVersion() != 1) {
    FUNCTION_MSG("Wrong XnGetVersion\n");
    FUNCTION_EXIT();
    return SP_RETURN_BAD_CONFIG;
  }
  RtlZeroMemory(xvsd, sizeof(XENVBD_SCSIPORT_DATA));

  access_range = &((*(ConfigInfo->AccessRanges))[0]);
  xvdd = (PXENVBD_DEVICE_DATA)(ULONG_PTR)access_range->RangeStart.QuadPart;
  xvsd->xvdd = xvdd;
  xvdd->xvsd = xvsd;

  InitializeListHead(&xvdd->srb_list);
  xvdd->aligned_buffer_in_use = FALSE;
  /* align the buffer to PAGE_SIZE */
  xvdd->aligned_buffer = (PVOID)((ULONG_PTR)((PUCHAR)xvsd->aligned_buffer_data + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
  KdPrint((__DRIVER_NAME "     aligned_buffer_data = %p\n", xvsd->aligned_buffer_data));
  KdPrint((__DRIVER_NAME "     aligned_buffer = %p\n", xvdd->aligned_buffer));

  ConfigInfo->MaximumTransferLength = 4 * 1024 * 1024; //BLKIF_MAX_SEGMENTS_PER_REQUEST * PAGE_SIZE;
  ConfigInfo->NumberOfPhysicalBreaks = ConfigInfo->MaximumTransferLength >> PAGE_SHIFT; //BLKIF_MAX_SEGMENTS_PER_REQUEST - 1;
  FUNCTION_MSG("ConfigInfo->MaximumTransferLength = %d\n", ConfigInfo->MaximumTransferLength);
  FUNCTION_MSG("ConfigInfo->NumberOfPhysicalBreaks = %d\n", ConfigInfo->NumberOfPhysicalBreaks);
  if (!dump_mode) {
    xvdd->aligned_buffer_size = BLKIF_MAX_SEGMENTS_PER_REQUEST * PAGE_SIZE;
  } else {
    xvdd->aligned_buffer_size = DUMP_MODE_UNALIGNED_PAGES * PAGE_SIZE;
  }
  //status = XenVbd_Connect(DeviceExtension, FALSE);

  FUNCTION_MSG("MultipleRequestPerLu = %d\n", ConfigInfo->MultipleRequestPerLu);
  FUNCTION_MSG("TaggedQueuing = %d\n", ConfigInfo->TaggedQueuing);
  FUNCTION_MSG("AutoRequestSense  = %d\n", ConfigInfo->AutoRequestSense );
  ConfigInfo->ScatterGather = TRUE;
  ConfigInfo->Master = TRUE;
  ConfigInfo->CachesData = FALSE;
  ConfigInfo->MapBuffers = TRUE;
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
XenVbd_HwScsiInitialize(PVOID DeviceExtension)
{
  PXENVBD_SCSIPORT_DATA xvsd = (PXENVBD_SCSIPORT_DATA)DeviceExtension;
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)xvsd->xvdd;
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
    /* nothing */
  } else {
    xvdd->grant_tag = (ULONG)'DUMP';
  }
  
  FUNCTION_EXIT();

  return TRUE;
}

/* this is only used during hiber and dump */
static BOOLEAN
XenVbd_HwScsiInterrupt(PVOID DeviceExtension)
{
  PXENVBD_SCSIPORT_DATA xvsd = DeviceExtension;
  XenVbd_HandleEvent(xvsd->xvdd);
  return TRUE;
}

static BOOLEAN
XenVbd_HwScsiResetBus(PVOID DeviceExtension, ULONG PathId)
{
  PXENVBD_SCSIPORT_DATA xvsd = DeviceExtension;
  return XenVbd_ResetBus(xvsd->xvdd, PathId);
}

static VOID
XenVbd_CompleteDisconnect(PXENVBD_DEVICE_DATA xvdd) {
  PXENVBD_SCSIPORT_DATA xvsd = (PXENVBD_SCSIPORT_DATA)xvdd->xvsd;
  PSCSI_REQUEST_BLOCK srb;
  
  if (xvsd->stop_srb) {
    srb = xvsd->stop_srb;
    xvsd->stop_srb = NULL;
    ScsiPortNotification(RequestComplete, xvsd, srb);
  }
}

static BOOLEAN
XenVbd_HwScsiStartIo(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK srb)
{
  PXENVBD_SCSIPORT_DATA xvsd = DeviceExtension;
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)xvsd->xvdd;
  PSRB_IO_CONTROL sic;

  //FUNCTION_ENTER();
  //FUNCTION_MSG("HwScsiStartIo Srb = %p, PathId = %d, TargetId = %d, Lun = %d\n", srb, srb->PathId, srb->TargetId, srb->Lun);
  if (srb->PathId != 0 || srb->TargetId != 0 || srb->Lun != 0) {
    FUNCTION_MSG("HwScsiStartIo (Out of bounds - PathId = %d, TargetId = %d, Lun = %d)\n", srb->PathId, srb->TargetId, srb->Lun);
    srb->SrbStatus = SRB_STATUS_NO_DEVICE;
    ScsiPortNotification(RequestComplete, xvsd, srb);
  } else if (srb->Function == SRB_FUNCTION_IO_CONTROL && memcmp(((PSRB_IO_CONTROL)srb->DataBuffer)->Signature, XENVBD_CONTROL_SIG, 8) == 0) {
    sic = srb->DataBuffer;
    switch(sic->ControlCode) {
    case XENVBD_CONTROL_EVENT:
      XenVbd_HandleEvent(xvdd);
      srb->SrbStatus = SRB_STATUS_SUCCESS;
      ScsiPortNotification(RequestComplete, xvsd, srb);
    case XENVBD_CONTROL_STOP:
      if (xvdd->shadow_free == SHADOW_ENTRIES) {
        srb->SrbStatus = SRB_STATUS_SUCCESS;
        ScsiPortNotification(RequestComplete, xvsd, srb);
      } else {
        xvsd->stop_srb = srb;
      }
      break;
    case XENVBD_CONTROL_START:
      XenVbd_ProcessSrbList(xvdd);
      break;
    default:
      FUNCTION_MSG("XENVBD_CONTROL_%d\n", sic->ControlCode);
      srb->SrbStatus = SRB_STATUS_SUCCESS;
      ScsiPortNotification(RequestComplete, xvsd, srb);
      break;
    }
  } else if (xvdd->device_state == DEVICE_STATE_INACTIVE) {
    FUNCTION_MSG("HwScsiStartIo Inactive Device (in StartIo)\n");
    srb->SrbStatus = SRB_STATUS_NO_DEVICE;
    ScsiPortNotification(RequestComplete, xvsd, srb);
  } else {
    xvsd->outstanding++;
    XenVbd_PutSrbOnList(xvdd, srb);
    XenVbd_ProcessSrbList(xvdd);
  }
  /* need 2 spare slots - 1 for EVENT and one for STOP/START */
  if (xvsd->outstanding < 30) {
    ScsiPortNotification(NextLuRequest, xvsd, srb->PathId, srb->TargetId, srb->Lun);
  } else {
    ScsiPortNotification(NextRequest, xvsd);
  }
  //FUNCTION_EXIT();
  return TRUE;
}

static SCSI_ADAPTER_CONTROL_STATUS
XenVbd_HwScsiAdapterControl(PVOID DeviceExtension, SCSI_ADAPTER_CONTROL_TYPE ControlType, PVOID Parameters)
{
  PXENVBD_SCSIPORT_DATA xvsd = DeviceExtension;
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)xvsd->xvdd;
  SCSI_ADAPTER_CONTROL_STATUS Status = ScsiAdapterControlSuccess;
  PSCSI_SUPPORTED_CONTROL_TYPE_LIST SupportedControlTypeList;
  //KIRQL OldIrql;

  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  KdPrint((__DRIVER_NAME "     xvsd = %p\n", xvsd));

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
    if (xvdd->device_state == DEVICE_STATE_INACTIVE) {
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
  HW_INITIALIZATION_DATA HwInitializationData;
  
  /* RegistryPath == NULL when we are invoked as a crash dump driver */
  if (!RegistryPath) {
    dump_mode = TRUE;
    // TODO: what if hibernate and not dump?
    XnDumpModeHookDebugPrint();
  }

  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  KdPrint((__DRIVER_NAME "     DriverObject = %p, RegistryPath = %p\n", DriverObject, RegistryPath));
  
  RtlZeroMemory(&HwInitializationData, sizeof(HW_INITIALIZATION_DATA));
  HwInitializationData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
  HwInitializationData.AdapterInterfaceType = PNPBus; /* not Internal */
  HwInitializationData.SrbExtensionSize = sizeof(srb_list_entry_t);
  HwInitializationData.NumberOfAccessRanges = 1;
  HwInitializationData.MapBuffers = TRUE;
  HwInitializationData.NeedPhysicalAddresses  = FALSE;
  HwInitializationData.TaggedQueuing = TRUE;
  HwInitializationData.AutoRequestSense = TRUE;
  HwInitializationData.MultipleRequestPerLu = TRUE;
  HwInitializationData.ReceiveEvent = FALSE;
  HwInitializationData.HwInitialize = XenVbd_HwScsiInitialize;
  HwInitializationData.HwStartIo = XenVbd_HwScsiStartIo;
  HwInitializationData.HwFindAdapter = XenVbd_HwScsiFindAdapter;
  HwInitializationData.HwResetBus = XenVbd_HwScsiResetBus;
  HwInitializationData.HwAdapterControl = XenVbd_HwScsiAdapterControl;
  if (!dump_mode) {
    HwInitializationData.DeviceExtensionSize = FIELD_OFFSET(XENVBD_SCSIPORT_DATA, aligned_buffer_data) + UNALIGNED_BUFFER_DATA_SIZE;
  } else {
    HwInitializationData.HwInterrupt = XenVbd_HwScsiInterrupt;
    HwInitializationData.DeviceExtensionSize = FIELD_OFFSET(XENVBD_SCSIPORT_DATA, aligned_buffer_data) + UNALIGNED_BUFFER_DATA_SIZE_DUMP_MODE;
  }
  status = ScsiPortInitialize(DriverObject, RegistryPath, &HwInitializationData, NULL);
  
  if(!NT_SUCCESS(status)) {
    FUNCTION_MSG("ScsiPortInitialize failed with status 0x%08x\n", status);
  }

  FUNCTION_EXIT();

  return status;
}
