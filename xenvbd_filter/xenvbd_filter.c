/*
PV Drivers for Windows Xen HVM Domains
Copyright (C) 2013 James Harper

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

#include "xenvbd_filter.h"
#include "ntddscsi.h"
#include "srb.h"

DRIVER_INITIALIZE DriverEntry;

static EVT_WDF_DRIVER_UNLOAD XenVbd_EvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD XenVbd_EvtDeviceAdd;

static VOID XenVbd_DeviceCallback(PVOID context, ULONG callback_type, PVOID value);
static VOID XenVbd_HandleEventDIRQL(PVOID context);
static VOID XenVbd_StopRing(PXENVBD_DEVICE_DATA xvdd, BOOLEAN suspend);
static VOID XenVbd_StartRing(PXENVBD_DEVICE_DATA xvdd, BOOLEAN suspend);

#include "../xenvbd_common/common_xen.h"

static VOID XenVbd_SendEvent(WDFDEVICE device);

static VOID
XenVbd_StopRing(PXENVBD_DEVICE_DATA xvdd, BOOLEAN suspend) {
  PXENVBD_FILTER_DATA xvfd = (PXENVBD_FILTER_DATA)xvdd->xvfd;
  NTSTATUS status;
  WDFREQUEST request;
  WDF_REQUEST_SEND_OPTIONS send_options;
  IO_STACK_LOCATION stack;
  SCSI_REQUEST_BLOCK srb;
  SRB_IO_CONTROL sic;

  FUNCTION_ENTER();
  
  /* send a 'stop' down if we are suspending */
  if (suspend) {
    status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, xvfd->wdf_target, &request);
    FUNCTION_MSG("WdfRequestCreate = %08x\n", status);

    RtlZeroMemory(&stack, sizeof(IO_STACK_LOCATION));
    stack.MajorFunction = IRP_MJ_SCSI;
    stack.MinorFunction = IRP_MN_SCSI_CLASS;
    stack.Parameters.Scsi.Srb = &srb;

    RtlZeroMemory(&srb, SCSI_REQUEST_BLOCK_SIZE);
    srb.SrbFlags = SRB_FLAGS_BYPASS_FROZEN_QUEUE | SRB_FLAGS_NO_QUEUE_FREEZE;
    srb.Length = SCSI_REQUEST_BLOCK_SIZE;
    srb.PathId = 0;
    srb.TargetId = 0;
    srb.Lun = 0;
    srb.OriginalRequest = WdfRequestWdmGetIrp(request);
    srb.Function = SRB_FUNCTION_IO_CONTROL;
    srb.DataBuffer = &sic;
    
    RtlZeroMemory(&sic, sizeof(SRB_IO_CONTROL));
    sic.HeaderLength = sizeof(SRB_IO_CONTROL);
    memcpy(sic.Signature, XENVBD_CONTROL_SIG, 8);
    sic.Timeout = 60;
    sic.ControlCode = XENVBD_CONTROL_STOP;
    
    WdfRequestWdmFormatUsingStackLocation(request, &stack);
    
    WDF_REQUEST_SEND_OPTIONS_INIT(&send_options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    if (!WdfRequestSend(request, xvfd->wdf_target, &send_options)) {
      FUNCTION_MSG("Request was _NOT_ sent\n");
    }
    status = WdfRequestGetStatus(request);
    FUNCTION_MSG("Request Status = %08x\n", status);
    FUNCTION_MSG("SRB Status = %08x\n", srb.SrbStatus);

    WdfObjectDelete(request);
  }
  
  status = XnWriteInt32(xvdd->handle, XN_BASE_FRONTEND, "state", XenbusStateClosing);

  FUNCTION_EXIT();
}

static VOID
XenVbd_StartRing(PXENVBD_DEVICE_DATA xvdd, BOOLEAN suspend) {
  PXENVBD_FILTER_DATA xvfd = (PXENVBD_FILTER_DATA)xvdd->xvfd;
  NTSTATUS status;
  WDFREQUEST request;
  WDF_REQUEST_SEND_OPTIONS send_options;
  IO_STACK_LOCATION stack;
  SCSI_REQUEST_BLOCK srb;
  SRB_IO_CONTROL sic;

  FUNCTION_ENTER();
  
  /* send a 'start' down if we are resuming from a suspend */
  if (suspend) {
    status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, xvfd->wdf_target, &request);
    FUNCTION_MSG("WdfRequestCreate = %08x\n", status);

    RtlZeroMemory(&stack, sizeof(IO_STACK_LOCATION));
    stack.MajorFunction = IRP_MJ_SCSI;
    stack.Parameters.Scsi.Srb = &srb;

    RtlZeroMemory(&srb, SCSI_REQUEST_BLOCK_SIZE);
    srb.SrbFlags = SRB_FLAGS_BYPASS_FROZEN_QUEUE | SRB_FLAGS_NO_QUEUE_FREEZE;
    srb.Length = SCSI_REQUEST_BLOCK_SIZE;
    srb.PathId = 0;
    srb.TargetId = 0;
    srb.Lun = 0;
    srb.OriginalRequest = WdfRequestWdmGetIrp(request);
    srb.Function = SRB_FUNCTION_IO_CONTROL;
    srb.DataBuffer = &sic;
    
    RtlZeroMemory(&sic, sizeof(SRB_IO_CONTROL));
    sic.HeaderLength = sizeof(SRB_IO_CONTROL);
    memcpy(sic.Signature, XENVBD_CONTROL_SIG, 8);
    sic.Timeout = 60;
    sic.ControlCode = XENVBD_CONTROL_START;
    
    WdfRequestWdmFormatUsingStackLocation(request, &stack);
    
    WDF_REQUEST_SEND_OPTIONS_INIT(&send_options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    if (!WdfRequestSend(request, xvfd->wdf_target, &send_options)) {
      FUNCTION_MSG("Request was _NOT_ sent\n");
    }
    status = WdfRequestGetStatus(request);
    FUNCTION_MSG("Request Status = %08x\n", status);
    FUNCTION_MSG("SRB Status = %08x\n", srb.SrbStatus);

    WdfObjectDelete(request);
  }
  
  FUNCTION_EXIT();
}

static VOID
XenVbd_SendRequestComplete(WDFREQUEST request, WDFIOTARGET target, PWDF_REQUEST_COMPLETION_PARAMS params, WDFCONTEXT context) {
  NTSTATUS status;
  PSCSI_REQUEST_BLOCK srb = context;

  UNREFERENCED_PARAMETER(target);
  UNREFERENCED_PARAMETER(params);
  UNREFERENCED_PARAMETER(context);

  status = WdfRequestGetStatus(request);
  if (status != 0 || srb->SrbStatus != SRB_STATUS_SUCCESS) {
    FUNCTION_MSG("Request Status = %08x, SRB Status = %08x\n", status, srb->SrbStatus);
  }
  ExFreePoolWithTag(context, XENVBD_POOL_TAG);
  WdfObjectDelete(request);
}

static VOID
XenVbd_SendEvent(WDFDEVICE device) {
  PXENVBD_FILTER_DATA xvfd = GetXvfd(device);
  NTSTATUS status;
  WDFREQUEST request;
  WDF_REQUEST_SEND_OPTIONS send_options;
  IO_STACK_LOCATION stack;
  PUCHAR buf;
  PSCSI_REQUEST_BLOCK srb;
  PSRB_IO_CONTROL sic;

  status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, xvfd->wdf_target, &request);

  buf = ExAllocatePoolWithTag(NonPagedPool, sizeof(SCSI_REQUEST_BLOCK) + sizeof(SRB_IO_CONTROL), XENVBD_POOL_TAG);
  RtlZeroMemory(buf, sizeof(SCSI_REQUEST_BLOCK) + sizeof(SRB_IO_CONTROL));
  srb = (PSCSI_REQUEST_BLOCK)(buf);
  sic = (PSRB_IO_CONTROL)(buf + sizeof(SCSI_REQUEST_BLOCK));
  
  srb->Length = sizeof(SCSI_REQUEST_BLOCK);
  srb->SrbFlags = SRB_FLAGS_BYPASS_FROZEN_QUEUE | SRB_FLAGS_NO_QUEUE_FREEZE;
  srb->PathId = 0;
  srb->TargetId = 0;
  srb->Lun = 0;
  srb->OriginalRequest = WdfRequestWdmGetIrp(request);
  srb->Function = SRB_FUNCTION_IO_CONTROL;
  srb->DataBuffer = sic;
  srb->DataTransferLength = sizeof(SCSI_REQUEST_BLOCK) + sizeof(SRB_IO_CONTROL);
  srb->TimeOutValue = (ULONG)-1;
  
  sic->HeaderLength = sizeof(SRB_IO_CONTROL);
  memcpy(sic->Signature, XENVBD_CONTROL_SIG, 8);
  sic->Timeout = (ULONG)-1;
  sic->ControlCode = XENVBD_CONTROL_EVENT;

  RtlZeroMemory(&stack, sizeof(IO_STACK_LOCATION));
  stack.MajorFunction = IRP_MJ_SCSI;
  stack.MinorFunction = IRP_MN_SCSI_CLASS;
  stack.Parameters.Scsi.Srb = srb;

  WdfRequestWdmFormatUsingStackLocation(request, &stack);
  WdfRequestSetCompletionRoutine(request, XenVbd_SendRequestComplete, buf);
  
  WDF_REQUEST_SEND_OPTIONS_INIT(&send_options, 0); //WDF_REQUEST_SEND_OPTION_IGNORE_TARGET_STATE);
  if (!WdfRequestSend(request, xvfd->wdf_target, &send_options)) {
    FUNCTION_MSG("Error sending request\n");
  }
}

static VOID
XenVbd_EvtDpcEvent(WDFDPC dpc) {
  WDFDEVICE device = WdfDpcGetParentObject(dpc);

  XenVbd_SendEvent(device);
}  

static VOID
XenVbd_HandleEventDIRQL(PVOID context) {
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)context;
  PXENVBD_FILTER_DATA xvfd = (PXENVBD_FILTER_DATA)xvdd->xvfd;
  WdfDpcEnqueue(xvfd->dpc);
}

static NTSTATUS
XenVbd_EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous_state) {
  PXENVBD_FILTER_DATA xvfd = GetXvfd(device);
  NTSTATUS status;
  
  UNREFERENCED_PARAMETER(previous_state);
  // if waking from hibernate then same as suspend... maybe?
  FUNCTION_ENTER();
  status = XenVbd_Connect(&xvfd->xvdd, FALSE);
  FUNCTION_EXIT();
  return status;
}

static NTSTATUS
XenVbd_EvtDeviceD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target_state) {
  PXENVBD_FILTER_DATA xvfd = GetXvfd(device);
  NTSTATUS status;
  // if hibernate then same as suspend
  UNREFERENCED_PARAMETER(target_state);
  FUNCTION_ENTER();
  status = XenVbd_Disconnect(&xvfd->xvdd, FALSE);
  FUNCTION_EXIT();
  return status;
}

static NTSTATUS
XenVbd_IoCompletion_START_DEVICE(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(irp);
  FUNCTION_ENTER();
  ExFreePoolWithTag(context, XENVBD_POOL_TAG);
  FUNCTION_EXIT();
  return STATUS_SUCCESS;
}

static NTSTATUS
XenVbd_EvtDeviceWdmIrpPreprocess_START_DEVICE(WDFDEVICE device, PIRP irp) {
  PXENVBD_FILTER_DATA xvfd = GetXvfd(device);
  PIO_STACK_LOCATION stack;
  PCM_RESOURCE_LIST crl;
  PCM_FULL_RESOURCE_DESCRIPTOR cfrd;
  PCM_PARTIAL_RESOURCE_LIST cprl;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR prd;

  FUNCTION_ENTER();

  /*
  Pass down the xvdd area as a memory resource. This gives xenvbd the data in a known place
  and also satisifies the scsiport requirement for a memory resource
  */
  IoCopyCurrentIrpStackLocationToNext(irp);
  stack = IoGetNextIrpStackLocation(irp);

  crl = ExAllocatePoolWithTag(NonPagedPool,
          FIELD_OFFSET(CM_RESOURCE_LIST, List) +
          FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR, PartialResourceList) +
          FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors) +
          sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) * 1, XENVBD_POOL_TAG);
  if (!crl) {
    // TODO: Fail this correctly
  }
  crl->Count = 1;
  cfrd = &crl->List[0];
  cfrd->InterfaceType = PNPBus;
  cfrd->BusNumber = 0;
  cprl = &cfrd->PartialResourceList;
  cprl->Version = 1;
  cprl->Revision = 1;
  cprl->Count = 1;
  prd = &cprl->PartialDescriptors[0];
  prd->Type = CmResourceTypeMemory;
  prd->ShareDisposition = CmResourceShareShared;
  prd->Flags = CM_RESOURCE_MEMORY_READ_WRITE | CM_RESOURCE_MEMORY_CACHEABLE;
  prd->u.Memory.Start.QuadPart = (ULONG_PTR)&xvfd->xvdd;
  prd->u.Memory.Length = sizeof(XENVBD_DEVICE_DATA);
  stack->Parameters.StartDevice.AllocatedResources = crl;
  stack->Parameters.StartDevice.AllocatedResourcesTranslated = crl;

  IoSetCompletionRoutine(irp, XenVbd_IoCompletion_START_DEVICE, crl, TRUE, TRUE, TRUE);

  FUNCTION_EXIT();

  return WdfDeviceWdmDispatchPreprocessedIrp(device, irp);
}

static NTSTATUS
XenVbd_EvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT device_init) {
  PXENVBD_FILTER_DATA xvfd;
  NTSTATUS status;
  WDFDEVICE device;
  WDF_OBJECT_ATTRIBUTES device_attributes;
  WDF_PNPPOWER_EVENT_CALLBACKS pnp_power_callbacks;
  WDF_DPC_CONFIG dpc_config;
  WDF_OBJECT_ATTRIBUTES oa;
  UCHAR pnp_minor_functions[] = { IRP_MN_START_DEVICE };
  
  UNREFERENCED_PARAMETER(driver);

  FUNCTION_ENTER();

  WdfDeviceInitSetDeviceType(device_init, FILE_DEVICE_UNKNOWN);

  WdfFdoInitSetFilter(device_init);

  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power_callbacks);
  pnp_power_callbacks.EvtDeviceD0Entry = XenVbd_EvtDeviceD0Entry;
  pnp_power_callbacks.EvtDeviceD0Exit = XenVbd_EvtDeviceD0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp_power_callbacks);

  status = WdfDeviceInitAssignWdmIrpPreprocessCallback(device_init, XenVbd_EvtDeviceWdmIrpPreprocess_START_DEVICE,
    IRP_MJ_PNP, pnp_minor_functions, ARRAY_SIZE(pnp_minor_functions));
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, XENVBD_FILTER_DATA);
  status = WdfDeviceCreate(&device_init, &device_attributes, &device);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("Error creating device 0x%x\n", status);
    return status;
  }

  xvfd = GetXvfd(device);
  xvfd->wdf_device = device;
  xvfd->wdf_target = WdfDeviceGetIoTarget(device);
  xvfd->xvdd.xvfd = xvfd;
  xvfd->xvdd.pdo = WdfDeviceWdmGetPhysicalDevice(device);
  xvfd->xvdd.grant_tag = XENVBD_POOL_TAG;

  KeInitializeEvent(&xvfd->xvdd.backend_event, SynchronizationEvent, FALSE);

  WDF_DPC_CONFIG_INIT(&dpc_config, XenVbd_EvtDpcEvent);
  WDF_OBJECT_ATTRIBUTES_INIT(&oa);
  oa.ParentObject = device;
  status = WdfDpcCreate(&dpc_config, &oa, &xvfd->dpc);

  WdfDeviceSetSpecialFileSupport(device, WdfSpecialFilePaging, TRUE);
  WdfDeviceSetSpecialFileSupport(device, WdfSpecialFileHibernation, TRUE);
  WdfDeviceSetSpecialFileSupport(device, WdfSpecialFileDump, TRUE);

  FUNCTION_EXIT();
  return status;
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
  NTSTATUS status;
  WDF_DRIVER_CONFIG config;
  WDFDRIVER driver;
  
  UNREFERENCED_PARAMETER(RegistryPath);

  FUNCTION_ENTER();

  WDF_DRIVER_CONFIG_INIT(&config, XenVbd_EvtDeviceAdd);
  status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("WdfDriverCreate failed with status 0x%x\n", status);
    FUNCTION_EXIT();
    return status;
  }
  FUNCTION_EXIT();
  return status;
}
