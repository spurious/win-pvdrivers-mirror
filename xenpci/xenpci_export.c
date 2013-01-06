/*
PV Drivers for Windows Xen HVM Domains
Copyright (C) 2012 James Harper

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

ULONG
XnGetVersion() {
  return 1;
}

static VOID
XnBackendStateCallback(char *path, PVOID context) {
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  PCHAR err;
  PCHAR value;
  ULONG backend_state;
  
  FUNCTION_ENTER();
  //RtlStringCbPrintfA(path, ARRAY_SIZE(path), "%s/state", xppdd->backend_path);
  FUNCTION_MSG("Read path=%s\n", path);
  err = XenBus_Read(xpdd, XBT_NIL, path, &value);
  if (err) {
    FUNCTION_MSG("Error %s\n", err);
    XenPci_FreeMem(err);
    /* this is pretty catastrophic... */
    /* maybe call the callback with an unknown or something... or just ignore? */
    FUNCTION_EXIT();
    return;
  }
  FUNCTION_MSG("Read value=%s\n", value);
  backend_state = atoi(value);
  XenPci_FreeMem(value);
  xppdd->backend_state_callback(xppdd->backend_state_callback_context, backend_state);
  FUNCTION_EXIT();
}


char *
XenBus_AddWatchx(
  PVOID Context,
  xenbus_transaction_t xbt,
  char *Path,
  PXN_WATCH_CALLBACK ServiceRoutine,
  PVOID ServiceContext);

XN_HANDLE
XnOpenDevice(PDEVICE_OBJECT pdo, PXN_BACKEND_STATE_CALLBACK callback, PVOID context) {
  WDFDEVICE device;
  PXENPCI_PDO_DEVICE_DATA xppdd;
  PXENPCI_DEVICE_DATA xpdd;
  PCHAR response;
  CHAR path[128];
  
  FUNCTION_ENTER();
  device = WdfWdmDeviceGetWdfDeviceHandle(pdo);
  if (!device) {
    FUNCTION_MSG("Failed to get WDFDEVICE for %p\n", pdo);
    return NULL;
  }
  xppdd = GetXppdd(device);
  xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  xppdd->backend_state_callback = callback;
  xppdd->backend_state_callback_context = context;
  RtlStringCbPrintfA(path, ARRAY_SIZE(path), "%s/state", xppdd->backend_path);
  response = XenBus_AddWatch(xpdd, XBT_NIL, path, XnBackendStateCallback, device);
  if (response) {
    FUNCTION_MSG("XnAddWatch - %s = %s\n", path, response);
    XenPci_FreeMem(response);
    xppdd->backend_state_callback = NULL;
    xppdd->backend_state_callback_context = NULL;
    FUNCTION_EXIT();
    return NULL;
  }

  FUNCTION_EXIT();
  return device;
}

VOID
XnCloseDevice(XN_HANDLE handle) {
  UNREFERENCED_PARAMETER(handle);
}

#if 0
NTSTATUS
XnAddWatch(XN_HANDLE handle, char *path, PXN_WATCH_CALLBACK callback, PVOID context) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  PCHAR response;
  NTSTATUS status;
  
  response = XenBus_AddWatch(xpdd, XBT_NIL, path, callback, context);
  if (response == NULL) {
    FUNCTION_MSG("XnAddWatch - %s = NULL\n", path);
    status = STATUS_SUCCESS;
  } else {
    FUNCTION_MSG("XnAddWatch - %s = %s\n", path, response);
    XenPci_FreeMem(response);
    status = STATUS_UNSUCCESSFUL;
  }
  return status;
}

NTSTATUS
XnRemoveWatch(XN_HANDLE handle, char *path, PXN_WATCH_CALLBACK callback, PVOID context) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  PCHAR response;
  NTSTATUS status;

  response = XenBus_RemWatch(xpdd, XBT_NIL, path, callback, context);
  if (response == NULL) {
    FUNCTION_MSG("XnRemoveWatch - %s = NULL\n", path);
    status = STATUS_SUCCESS;
  } else {
    FUNCTION_MSG("XnRemoveWatch - %s = %s\n", path, response);
    XenPci_FreeMem(response);
    status = STATUS_UNSUCCESSFUL;
  }
}
#endif

evtchn_port_t
XnAllocateEvent(XN_HANDLE handle) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return EvtChn_AllocUnbound(xpdd, xppdd->backend_id);
}

VOID
XnFreeEvent(XN_HANDLE handle, evtchn_port_t port) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  EvtChn_Close(xpdd, port);
  return;
}

NTSTATUS
XnBindEvent(XN_HANDLE handle, evtchn_port_t port, PXN_EVENT_CALLBACK callback, PVOID context) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return EvtChn_Bind(xpdd, port, callback, context, EVT_ACTION_FLAGS_DEFAULT);
}

NTSTATUS
XnUnBindEvent(XN_HANDLE handle, evtchn_port_t port) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return EvtChn_Unbind(xpdd, port);
}

grant_ref_t
XnGrantAccess(XN_HANDLE handle, uint32_t frame, int readonly, grant_ref_t ref, ULONG tag)
{
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return GntTbl_GrantAccess(xpdd, xppdd->backend_id, frame, readonly, ref, tag);
}

BOOLEAN
XnEndAccess(XN_HANDLE handle, grant_ref_t ref, BOOLEAN keepref, ULONG tag)
{
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return GntTbl_EndAccess(xpdd, ref, keepref, tag);
}

grant_ref_t
XnAllocateGrant(XN_HANDLE handle, ULONG tag) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return GntTbl_GetRef(xpdd, tag);
}

VOID
XnFreeGrant(XN_HANDLE handle, grant_ref_t ref, ULONG tag) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  GntTbl_PutRef(xpdd, ref, tag);
}

/* result must be freed with XnFreeMem() */
NTSTATUS
XnReadString(XN_HANDLE handle, ULONG base, PCHAR path, PCHAR *value) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  PCHAR response;
  CHAR full_path[1024];
  
  switch(base) {
  case XN_BASE_FRONTEND:
    strncpy(full_path, xppdd->path, 1024);
    break;
  case XN_BASE_BACKEND:
    strncpy(full_path, xppdd->backend_path, 1024);
    break;
  case XN_BASE_GLOBAL:
    strncpy(full_path, "", 1024);
  }
  strncat(full_path, "/", 1024);
  strncat(full_path, path, 1024);
  
  response = XenBus_Read(xpdd, XBT_NIL, full_path, value);
  if (response) {
    FUNCTION_MSG("Error reading shutdown path - %s\n", response);
    XenPci_FreeMem(response);
    FUNCTION_EXIT();
    return STATUS_UNSUCCESSFUL;
  }
  return STATUS_SUCCESS;
}

NTSTATUS
XnWriteString(XN_HANDLE handle, ULONG base, PCHAR path, PCHAR value) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  PCHAR response;
  CHAR full_path[1024];

  switch(base) {
  case XN_BASE_FRONTEND:
    strncpy(full_path, xppdd->path, 1024);
    break;
  case XN_BASE_BACKEND:
    strncpy(full_path, xppdd->backend_path, 1024);
    break;
  case XN_BASE_GLOBAL:
    strncpy(full_path, "", 1024);
  }
  strncat(full_path, "/", 1024);
  strncat(full_path, path, 1024);
  FUNCTION_MSG("XnWriteString(%s, %s)\n", full_path, value);
  response = XenBus_Write(xpdd, XBT_NIL, full_path, value);
  if (response) {
    FUNCTION_MSG("XnWriteString - %s = %s\n", full_path, response);
    XenPci_FreeMem(response);
    FUNCTION_EXIT();
    return STATUS_UNSUCCESSFUL;
  }
  return STATUS_SUCCESS;
}

NTSTATUS
XnReadInt32(XN_HANDLE handle, ULONG base, PCHAR path, ULONG *value) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  CHAR full_path[1024];
  PCHAR response;
  PCHAR string_value;

  switch(base) {
  case XN_BASE_FRONTEND:
    strncpy(full_path, xppdd->path, 1024);
    break;
  case XN_BASE_BACKEND:
    strncpy(full_path, xppdd->backend_path, 1024);
    break;
  case XN_BASE_GLOBAL:
    strncpy(full_path, "", 1024);
  }
  strncat(full_path, "/", 1024);
  strncat(full_path, path, 1024);
  response = XenBus_Read(xpdd, XBT_NIL, full_path, &string_value);
  if (response) {
    FUNCTION_MSG("XnReadInt - %s = %s\n", full_path, response);
    XenPci_FreeMem(response);
    FUNCTION_EXIT();
    return STATUS_UNSUCCESSFUL;
  }
  *value = atoi(string_value);
  return STATUS_SUCCESS;
}

NTSTATUS
XnWriteInt32(XN_HANDLE handle, ULONG base, PCHAR path, ULONG value) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  CHAR full_path[1024];
  PCHAR response;
  
  switch(base) {
  case XN_BASE_FRONTEND:
    strncpy(full_path, xppdd->path, 1024);
    break;
  case XN_BASE_BACKEND:
    strncpy(full_path, xppdd->backend_path, 1024);
    break;
  case XN_BASE_GLOBAL:
    strncpy(full_path, "", 1024);
  }
  strncat(full_path, "/", 1024);
  strncat(full_path, path, 1024);
  
  FUNCTION_MSG("XnWriteInt32(%s, %d)\n", full_path, value);
  response = XenBus_Printf(xpdd, XBT_NIL, full_path, "%d", value);
  if (response) {
    FUNCTION_MSG("XnWriteInt - %s = %s\n", full_path, response);
    XenPci_FreeMem(response);
    FUNCTION_EXIT();
    return STATUS_UNSUCCESSFUL;
  }
  return STATUS_SUCCESS;
}

NTSTATUS
XnReadInt64(XN_HANDLE handle, ULONG base, PCHAR path, ULONGLONG *value) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  CHAR full_path[1024];
  PCHAR response;
  PCHAR string_value;
  PCHAR ptr;

  switch(base) {
  case XN_BASE_FRONTEND:
    strncpy(full_path, xppdd->path, 1024);
    break;
  case XN_BASE_BACKEND:
    strncpy(full_path, xppdd->backend_path, 1024);
    break;
  case XN_BASE_GLOBAL:
    strncpy(full_path, "", 1024);
    break;
  }
  strncat(full_path, "/", 1024);
  strncat(full_path, path, 1024);
  response = XenBus_Read(xpdd, XBT_NIL, full_path, &string_value);
  if (response) {
    FUNCTION_MSG("XnReadInt - %s = %s\n", full_path, response);
    XenPci_FreeMem(response);
    FUNCTION_EXIT();
    return STATUS_UNSUCCESSFUL;
  }
  *value = 0;
  for (ptr = string_value; *ptr && *ptr >= '0' && *ptr <= '9'; ptr++) {
    *value *= 10;
    *value += (*ptr) - '0';
  }
  return STATUS_SUCCESS;
}

NTSTATUS
XnWriteInt64(XN_HANDLE handle, ULONG base, PCHAR path, ULONGLONG value) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  CHAR full_path[1024];
  PCHAR response;
  
  switch(base) {
  case XN_BASE_FRONTEND:
    strncpy(full_path, xppdd->path, 1024);
    break;
  case XN_BASE_BACKEND:
    strncpy(full_path, xppdd->backend_path, 1024);
    break;
  case XN_BASE_GLOBAL:
    strncpy(full_path, "", 1024);
  }
  strncat(full_path, "/", 1024);
  strncat(full_path, path, 1024);
  
  response = XenBus_Printf(xpdd, XBT_NIL, full_path, "%I64d", value);
  if (response) {
    FUNCTION_MSG("XnWriteInt - %s = %s\n", full_path, response);
    XenPci_FreeMem(response);
    FUNCTION_EXIT();
    return STATUS_UNSUCCESSFUL;
  }
  return STATUS_SUCCESS;
}

NTSTATUS
XnNotify(XN_HANDLE handle, evtchn_port_t port) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  
  return EvtChn_Notify(xpdd, port);
}

/* called at PASSIVE_LEVEL */
VOID
XnGetValue(XN_HANDLE handle, ULONG value_type, PVOID value) {
  WDFDEVICE device = handle;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  //PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  DECLARE_UNICODE_STRING_SIZE(my_device_name, 128);
  ULONG i;

  switch (value_type) {
  case XN_VALUE_TYPE_QEMU_HIDE_FLAGS:
    *(PULONG)value = (ULONG)qemu_hide_flags_value;
    break;
  case XN_VALUE_TYPE_QEMU_FILTER:
    *(PULONG)value = FALSE;
    RtlUnicodeStringPrintf(&my_device_name, L"#%S#", xppdd->device);
    for (i = 0; i < WdfCollectionGetCount(qemu_hide_devices); i++) {
      WDFSTRING wdf_string = WdfCollectionGetItem(qemu_hide_devices, i);
      UNICODE_STRING hide_device_name;
      WdfStringGetUnicodeString(wdf_string, &hide_device_name);
      if (RtlCompareUnicodeString(&hide_device_name, &my_device_name, TRUE) != 0) {
        *(PULONG)value = TRUE;
        break;
      }
    }
    break;
  default:
    FUNCTION_MSG("GetValue unknown type %d\n", value_type);
    break;
  }
}

#if 0
static NTSTATUS
XenConfig_InitConfigPage(WDFDEVICE device)
{
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  //PXENCONFIG_DEVICE_DATA xcdd = (PXENCONFIG_DEVICE_DATA)device_object->DeviceExtension;
  //PXENPCI_PDO_DEVICE_DATA xppdd = (PXENPCI_PDO_DEVICE_DATA)device_object->DeviceExtension;
  //PXENPCI_DEVICE_DATA xpdd = xppdd->bus_fdo->DeviceExtension;
  PUCHAR ptr;
  PDEVICE_OBJECT curr, prev;
  PDRIVER_OBJECT fdo_driver_object;
  PUCHAR fdo_driver_extension;
  
  FUNCTION_ENTER();
  
  ptr = MmGetMdlVirtualAddress(xppdd->config_page_mdl);
  curr = IoGetAttachedDeviceReference(WdfDeviceWdmGetDeviceObject(device));
  //curr = WdfDeviceWdmGetAttachedDevice(device);
  while (curr != NULL)
  {
    fdo_driver_object = curr->DriverObject;
    KdPrint((__DRIVER_NAME "     fdo_driver_object = %p\n", fdo_driver_object));
    if (fdo_driver_object)
    {
      fdo_driver_extension = IoGetDriverObjectExtension(fdo_driver_object, UlongToPtr(XEN_INIT_DRIVER_EXTENSION_MAGIC));
      KdPrint((__DRIVER_NAME "     fdo_driver_extension = %p\n", fdo_driver_extension));
      if (fdo_driver_extension)
      {
        memcpy(ptr, fdo_driver_extension, PAGE_SIZE);
        ObDereferenceObject(curr);
        break;
      }
    }
    prev = curr;
    curr = IoGetLowerDeviceObject(curr);
    ObDereferenceObject(prev);
  }
  
  FUNCTION_EXIT();
  
  return STATUS_SUCCESS;
}

static NTSTATUS
XenPci_EvtChn_Bind(PVOID context, evtchn_port_t Port, PXEN_EVTCHN_SERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  
  return EvtChn_Bind(xpdd, Port, ServiceRoutine, ServiceContext, EVT_ACTION_FLAGS_DEFAULT);
}

static NTSTATUS
XenPci_EvtChn_BindDpc(PVOID context, evtchn_port_t Port, PXEN_EVTCHN_SERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  
  return EvtChn_BindDpc(xpdd, Port, ServiceRoutine, ServiceContext, EVT_ACTION_FLAGS_DEFAULT);
}

static NTSTATUS
XenPci_EvtChn_Unbind(PVOID context, evtchn_port_t Port)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  
  return EvtChn_Unbind(xpdd, Port);
}

static NTSTATUS
XenPci_EvtChn_Mask(PVOID context, evtchn_port_t Port)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  
  return EvtChn_Mask(xpdd, Port);
}

static NTSTATUS
XenPci_EvtChn_Unmask(PVOID context, evtchn_port_t Port)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  
  return EvtChn_Unmask(xpdd, Port);
}

static BOOLEAN
XenPci_EvtChn_AckEvent(PVOID context, evtchn_port_t port, BOOLEAN *last_interrupt)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  
  return EvtChn_AckEvent(xpdd, port, last_interrupt);
}

typedef struct {
  PXEN_EVTCHN_SYNC_ROUTINE sync_routine;
  PVOID sync_context;
} sync_context_t;

static BOOLEAN
XenPci_EvtChn_Sync_Routine(WDFINTERRUPT interrupt, WDFCONTEXT context)
{
  sync_context_t *wdf_sync_context = context;
  UNREFERENCED_PARAMETER(interrupt);
  return wdf_sync_context->sync_routine(wdf_sync_context->sync_context);
}

static BOOLEAN
XenPci_EvtChn_Sync(PVOID context, PXEN_EVTCHN_SYNC_ROUTINE sync_routine, PVOID sync_context)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  sync_context_t wdf_sync_context;
  
  wdf_sync_context.sync_routine = sync_routine;
  wdf_sync_context.sync_context = sync_context;
  
  return WdfInterruptSynchronize(xpdd->interrupt, XenPci_EvtChn_Sync_Routine, &wdf_sync_context);
}


PCHAR
XenPci_XenBus_Read(PVOID context, xenbus_transaction_t xbt, char *path, char **value)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return XenBus_Read(xpdd, xbt, path, value);
}

PCHAR
XenPci_XenBus_Write(PVOID context, xenbus_transaction_t xbt, char *path, char *value)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return XenBus_Write(xpdd, xbt, path, value);
}

PCHAR
XenPci_XenBus_Printf(PVOID context, xenbus_transaction_t xbt, char *path, char *fmt, ...)
{
  //PXENPCI_PDO_DEVICE_DATA xppdd = Context;
  //PXENPCI_DEVICE_DATA xpdd = xppdd->bus_fdo->DeviceExtension;
  //return XenBus_Printf(xpdd, xbt, path, value);
  UNREFERENCED_PARAMETER(context);
  UNREFERENCED_PARAMETER(xbt);
  UNREFERENCED_PARAMETER(path);
  UNREFERENCED_PARAMETER(fmt);
  return NULL;
}

PCHAR
XenPci_XenBus_StartTransaction(PVOID context, xenbus_transaction_t *xbt)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return XenBus_StartTransaction(xpdd, xbt);
}

PCHAR
XenPci_XenBus_EndTransaction(PVOID context, xenbus_transaction_t xbt, int abort, int *retry)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return XenBus_EndTransaction(xpdd, xbt, abort, retry);
}

PCHAR
XenPci_XenBus_List(PVOID context, xenbus_transaction_t xbt, char *prefix, char ***contents)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  return XenBus_List(xpdd, xbt, prefix, contents);
}

#endif
