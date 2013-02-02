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
#include <stdlib.h>
#include <io/ring.h>

#pragma warning(disable : 4200) // zero-sized array
#pragma warning(disable: 4127) // conditional expression is constant

/* Not really necessary but keeps PREfast happy */
static EVT_WDF_INTERRUPT_SYNCHRONIZE XenPci_EvtChn_Sync_Routine;
static EVT_WDF_DEVICE_D0_ENTRY XenPciPdo_EvtDeviceD0Entry;
static EVT_WDF_DEVICE_D0_EXIT XenPciPdo_EvtDeviceD0Exit;
static EVT_WDF_DEVICE_PREPARE_HARDWARE XenPciPdo_EvtDevicePrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE XenPciPdo_EvtDeviceReleaseHardware;
static EVT_WDF_DEVICE_USAGE_NOTIFICATION XenPciPdo_EvtDeviceUsageNotification;
//static EVT_WDFDEVICE_WDM_IRP_PREPROCESS XenPciPdo_EvtDeviceWdmIrpPreprocess_START_DEVICE;
//static EVT_WDF_DEVICE_RESOURCE_REQUIREMENTS_QUERY XenPciPdo_EvtDeviceResourceRequirementsQuery;
static EVT_WDF_DEVICE_PNP_STATE_CHANGE_NOTIFICATION XenPci_EvtDevicePnpStateChange;


      
/*
Called at PASSIVE_LEVEL(?)
Called during restore
*/
static ULONG
XenPci_ReadBackendState(PXENPCI_PDO_DEVICE_DATA xppdd)
{
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  char path[128];
  char *value;
  char *err;
  ULONG backend_state;
  
  RtlStringCbPrintfA(path, ARRAY_SIZE(path), "%s/state", xppdd->backend_path);
  err = XenBus_Read(xpdd, XBT_NIL, path, &value);
  if (err)
  {
    XenPci_FreeMem(err);
    return XenbusStateUnknown;
  }
  else
  {
    backend_state = atoi(value);
    XenPci_FreeMem(value);
    return backend_state;
  }
}

static VOID
XenPci_UpdateBackendState(PVOID context)
{
  WDFDEVICE device = context;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  ULONG new_backend_state;

  FUNCTION_ENTER();

  ExAcquireFastMutex(&xppdd->backend_state_mutex);

  new_backend_state = XenPci_ReadBackendState(xppdd);
  if (new_backend_state == XenbusStateUnknown)
  {
    if (xpdd->suspend_state != SUSPEND_STATE_NONE)
    {
      ExReleaseFastMutex(&xppdd->backend_state_mutex);
      return;
    }
    KdPrint(("Failed to read path, assuming closed\n"));
    new_backend_state = XenbusStateClosed;
  }

  if (xppdd->backend_state == new_backend_state)
  {
    KdPrint((__DRIVER_NAME "     state unchanged\n"));
    ExReleaseFastMutex(&xppdd->backend_state_mutex);
    return;
  }
  
  xppdd->backend_state = new_backend_state;

  switch (xppdd->backend_state)
  {
  case XenbusStateUnknown:
    KdPrint((__DRIVER_NAME "     Backend State Changed to Unknown\n"));
    break;

  case XenbusStateInitialising:
    KdPrint((__DRIVER_NAME "     Backend State Changed to Initialising\n"));
    break;

  case XenbusStateInitWait:
    KdPrint((__DRIVER_NAME "     Backend State Changed to InitWait\n"));
    break;

  case XenbusStateInitialised:
    KdPrint((__DRIVER_NAME "     Backend State Changed to Initialised\n"));
    break;

  case XenbusStateConnected:
    KdPrint((__DRIVER_NAME "     Backend State Changed to Connected\n"));  
    break;

  case XenbusStateClosing:
    KdPrint((__DRIVER_NAME "     Backend State Changed to Closing\n"));
    if (xppdd->frontend_state != XenbusStateClosing)
    {
      xppdd->backend_initiated_remove = TRUE;
      KdPrint((__DRIVER_NAME "     Requesting eject\n"));
      WdfPdoRequestEject(device);
    }
    break;

  case XenbusStateClosed:
    KdPrint((__DRIVER_NAME "     Backend State Changed to Closed\n"));
    break;
  
  default:
    KdPrint((__DRIVER_NAME "     Backend State Changed to Undefined = %d\n", xppdd->backend_state));
    break;
  }

  KeSetEvent(&xppdd->backend_state_event, 1, FALSE);

  ExReleaseFastMutex(&xppdd->backend_state_mutex);
  FUNCTION_EXIT();

  return;
}

static VOID
XenPci_BackendStateHandler(char *path, PVOID context)
{
  UNREFERENCED_PARAMETER(path);

  /* check that path == device/id/state */
  //RtlStringCbPrintfA(path, ARRAY_SIZE(path), "%s/state", xppdd->path);

  XenPci_UpdateBackendState(context);
}

static NTSTATUS
XenPci_GetBackendDetails(WDFDEVICE device)
{
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  char path[128];
  PCHAR res;
  PCHAR value;

  FUNCTION_ENTER();
  /* Get backend path */
  RtlStringCbPrintfA(path, ARRAY_SIZE(path),
    "%s/backend", xppdd->path);
  res = XenBus_Read(xpdd, XBT_NIL, path, &value);
  if (res)
  {
    KdPrint((__DRIVER_NAME "    Failed to read backend path\n"));
    XenPci_FreeMem(res);
    return STATUS_UNSUCCESSFUL;
  }
  RtlStringCbCopyA(xppdd->backend_path, ARRAY_SIZE(xppdd->backend_path), value);
  XenPci_FreeMem(value);

  /* Get backend id */
  RtlStringCbPrintfA(path, ARRAY_SIZE(path),
    "%s/backend-id", xppdd->path);
  res = XenBus_Read(xpdd, XBT_NIL, path, &value);
  if (res) {
    KdPrint((__DRIVER_NAME "    Failed to read backend id\n"));
    XenPci_FreeMem(res);
    return STATUS_UNSUCCESSFUL;
  }
  xppdd->backend_id = (domid_t)atoi(value);
  XenPci_FreeMem(value);
  FUNCTION_EXIT();  
  return STATUS_SUCCESS;
}

NTSTATUS
XenPci_SuspendPdo(WDFDEVICE device) {
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = xppdd->xpdd;
  PCHAR response;
  CHAR path[128];

  if (xppdd->device_callback) {
    FUNCTION_MSG("Suspending %s\n", xppdd->device);
    xppdd->device_callback(xppdd->device_callback_context, XN_DEVICE_CALLBACK_SUSPEND, NULL);
    RtlStringCbPrintfA(path, ARRAY_SIZE(path), "%s/state", xppdd->backend_path);
    response = XenBus_RemWatch(xpdd, XBT_NIL, path, XenPci_BackendStateCallback, xppdd);
    if (response) {
      FUNCTION_MSG("XnRemWatch - %s = %s\n", path, response);
      XenPci_FreeMem(response);
    }
  }
  return STATUS_SUCCESS;
}

NTSTATUS
XenPci_ResumePdo(WDFDEVICE device) {
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = xppdd->xpdd;
  PCHAR response;
  CHAR path[128];

  XenPci_GetBackendDetails(device);
  if (xppdd->device_callback) {
    FUNCTION_MSG("Resuming %s\n", xppdd->device);
    RtlStringCbPrintfA(path, ARRAY_SIZE(path), "%s/state", xppdd->backend_path);
    response = XenBus_AddWatch(xpdd, XBT_NIL, path, XenPci_BackendStateCallback, xppdd);
    if (response) {
      FUNCTION_MSG("XnAddWatch - %s = %s\n", path, response);
      XenPci_FreeMem(response);
      xppdd->device_callback = NULL;
      xppdd->device_callback_context = NULL;
      FUNCTION_EXIT();
      return STATUS_UNSUCCESSFUL;
    }
    xppdd->device_callback(xppdd->device_callback_context, XN_DEVICE_CALLBACK_RESUME, NULL);
  }
  return STATUS_SUCCESS;
}

NTSTATUS
XenPciPdo_EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous_state)
{
  NTSTATUS status = STATUS_SUCCESS;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  CHAR path[128];
  
  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     path = %s\n", xppdd->path));

  switch (previous_state)
  {
  case WdfPowerDeviceD0:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD1\n"));
    break;
  case WdfPowerDeviceD1:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD1\n"));
    break;
  case WdfPowerDeviceD2:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD2\n"));
    break;
  case WdfPowerDeviceD3:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD3\n"));
    if (xppdd->hiber_usage_kludge)
    {
      KdPrint((__DRIVER_NAME "     (but really WdfPowerDevicePrepareForHibernation)\n"));
      previous_state = WdfPowerDevicePrepareForHibernation;
    }
    break;
  case WdfPowerDeviceD3Final:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD3Final\n"));
    break;
  case WdfPowerDevicePrepareForHibernation:
    KdPrint((__DRIVER_NAME "     WdfPowerDevicePrepareForHibernation\n"));
    break;  
  default:
    KdPrint((__DRIVER_NAME "     Unknown WdfPowerDevice state %d\n", previous_state));
    break;  
  }

  status = XenPci_GetBackendDetails(device);
  if (!NT_SUCCESS(status))
  {
    WdfDeviceSetFailed(device, WdfDeviceFailedNoRestart);
    FUNCTION_EXIT_STATUS(status);
    return status;
  }

  if (previous_state == WdfPowerDeviceD3 || previous_state == WdfPowerDeviceD3Final)
  {
#if 0
    xppdd->requested_resources_ptr = xppdd->requested_resources_start;
    xppdd->assigned_resources_start = xppdd->assigned_resources_ptr = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, XENPCI_POOL_TAG);
    XenConfig_InitConfigPage(device);
    status = XenPci_XenConfigDevice(device);
#endif
  }
  else if (previous_state == WdfPowerDevicePrepareForHibernation)
  {
#if 0
    PVOID src, dst;
    
    ADD_XEN_INIT_REQ(&xppdd->requested_resources_ptr, XEN_INIT_TYPE_END, NULL, NULL, NULL);
    src = xppdd->requested_resources_start;
    xppdd->requested_resources_ptr = xppdd->requested_resources_start = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, XENPCI_POOL_TAG);
    xppdd->assigned_resources_ptr = xppdd->assigned_resources_start;

    dst = MmMapIoSpace(xppdd->config_page_phys, xppdd->config_page_length, MmNonCached);

    status = XenPci_XenConfigDeviceSpecifyBuffers(device, src, dst);

    MmUnmapIoSpace(dst, xppdd->config_page_length);
    ExFreePoolWithTag(src, XENPCI_POOL_TAG);
#endif
  }

  if (!NT_SUCCESS(status))
  {
    RtlStringCbPrintfA(path, ARRAY_SIZE(path), "%s/state", xppdd->backend_path);
    XenBus_RemWatch(xpdd, XBT_NIL, path, XenPci_BackendStateHandler, device);
    WdfDeviceSetFailed(device, WdfDeviceFailedNoRestart);
    FUNCTION_EXIT_STATUS(status);
    return status;
  }

  FUNCTION_EXIT();
  
  return status;
}

NTSTATUS
XenPciPdo_EvtDeviceD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target_state)
{
  NTSTATUS status = STATUS_SUCCESS;
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
  char path[128];
  
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(target_state);
  
  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     path = %s\n", xppdd->path));
  
  switch (target_state)
  {
  case WdfPowerDeviceD0:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD1\n"));
    break;
  case WdfPowerDeviceD1:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD1\n"));
    break;
  case WdfPowerDeviceD2:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD2\n"));
    break;
  case WdfPowerDeviceD3:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD3\n"));
    if (xppdd->hiber_usage_kludge)
    {
      KdPrint((__DRIVER_NAME "     (but really WdfPowerDevicePrepareForHibernation)\n"));
      target_state = WdfPowerDevicePrepareForHibernation;
    }
    break;
  case WdfPowerDeviceD3Final:
    KdPrint((__DRIVER_NAME "     WdfPowerDeviceD3Final\n"));
    break;
  case WdfPowerDevicePrepareForHibernation:
    KdPrint((__DRIVER_NAME "     WdfPowerDevicePrepareForHibernation\n"));
    break;  
  default:
    KdPrint((__DRIVER_NAME "     Unknown WdfPowerDevice state %d\n", target_state));
    break;  
  }
  
  if (target_state == WdfPowerDevicePrepareForHibernation)
  {
    KdPrint((__DRIVER_NAME "     not powering down as we are hibernating\n"));
    // should we set the backend state here so it's correct on resume???
  }
  else
  {
#if 0
    status = XenPci_XenShutdownDevice(device);
#endif
  }
  
  /* Remove watch on backend state */
  RtlStringCbPrintfA(path, ARRAY_SIZE(path), "%s/state", xppdd->backend_path);
  XenBus_RemWatch(xpdd, XBT_NIL, path, XenPci_BackendStateHandler, device);
  
  FUNCTION_EXIT();
  
  return status;
}

NTSTATUS
XenPciPdo_EvtDevicePrepareHardware(WDFDEVICE device, WDFCMRESLIST resources_raw, WDFCMRESLIST resources_translated)
{
  NTSTATUS status = STATUS_SUCCESS;

  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(resources_raw);
  UNREFERENCED_PARAMETER(resources_translated);
  
  FUNCTION_ENTER();
  FUNCTION_EXIT();
  
  return status;
}

NTSTATUS
XenPciPdo_EvtDeviceReleaseHardware(WDFDEVICE device, WDFCMRESLIST resources_translated)
{
  NTSTATUS status = STATUS_SUCCESS;
  
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(resources_translated);
  
  FUNCTION_ENTER();
  FUNCTION_EXIT();
  
  return status;
}

static VOID
XenPciPdo_EvtDeviceUsageNotification(WDFDEVICE device, WDF_SPECIAL_FILE_TYPE notification_type, BOOLEAN is_in_notification_path)
{
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);

  FUNCTION_ENTER();
  
  KdPrint((__DRIVER_NAME "     path = %s\n", xppdd->path));
  switch (notification_type)
  {
  case WdfSpecialFilePaging:
    KdPrint((__DRIVER_NAME "     notification_type = Paging, flag = %d\n", is_in_notification_path));
    break;
  case WdfSpecialFileHibernation:
    xppdd->hiber_usage_kludge = is_in_notification_path;
    KdPrint((__DRIVER_NAME "     notification_type = Hibernation, flag = %d\n", is_in_notification_path));
    break;
  case WdfSpecialFileDump:
    KdPrint((__DRIVER_NAME "     notification_type = Dump, flag = %d\n", is_in_notification_path));
    break;
  default:
    KdPrint((__DRIVER_NAME "     notification_type = %d, flag = %d\n", notification_type, is_in_notification_path));
    break;
  }

  FUNCTION_EXIT();
}

static VOID
XenPci_EvtDevicePnpStateChange(WDFDEVICE device, PCWDF_DEVICE_PNP_NOTIFICATION_DATA notification_data)
{
  PXENPCI_PDO_DEVICE_DATA xppdd = GetXppdd(device);
  
  //FUNCTION_ENTER();
  
  if (xppdd->backend_initiated_remove
    && notification_data->Type == StateNotificationEnterState
    && notification_data->Data.EnterState.CurrentState == WdfDevStatePnpQueryRemovePending 
    && notification_data->Data.EnterState.NewState == WdfDevStatePnpQueryCanceled)
  {
    PXENPCI_DEVICE_DATA xpdd = GetXpdd(xppdd->wdf_device_bus_fdo);
    
    KdPrint((__DRIVER_NAME "     Eject failed, doing surprise removal\n"));
    xppdd->do_not_enumerate = TRUE;
    XenPci_EvtChildListScanForChildren(xpdd->child_list);
  }
  
  //FUNCTION_EXIT();
  
  //return STATUS_SUCCESS;
}

NTSTATUS
XenPci_EvtChildListCreateDevice(WDFCHILDLIST child_list,
  PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER identification_header,
  PWDFDEVICE_INIT child_init) {
  NTSTATUS status = STATUS_SUCCESS;
  WDF_OBJECT_ATTRIBUTES child_attributes;
  WDFDEVICE child_device;
  PXENPCI_PDO_IDENTIFICATION_DESCRIPTION identification = (PXENPCI_PDO_IDENTIFICATION_DESCRIPTION)identification_header;
  WDF_DEVICE_PNP_CAPABILITIES child_pnp_capabilities;
  DECLARE_UNICODE_STRING_SIZE(buffer, 512);
  DECLARE_CONST_UNICODE_STRING(location, L"Xen Bus");
  PXENPCI_PDO_DEVICE_DATA xppdd;
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(WdfChildListGetDevice(child_list));
  //WDF_PDO_EVENT_CALLBACKS pdo_callbacks;
  WDF_PNPPOWER_EVENT_CALLBACKS child_pnp_power_callbacks;
  //UCHAR pnp_minor_functions[] = { IRP_MN_START_DEVICE };
  WDF_DEVICE_POWER_CAPABILITIES child_power_capabilities;
  
  FUNCTION_ENTER();

  WdfDeviceInitSetDeviceType(child_init, FILE_DEVICE_UNKNOWN);
  
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&child_pnp_power_callbacks);
  child_pnp_power_callbacks.EvtDeviceD0Entry = XenPciPdo_EvtDeviceD0Entry;
  //child_pnp_power_callbacks.EvtDeviceD0EntryPostInterruptsEnabled = XenPciPdo_EvtDeviceD0EntryPostInterruptsEnabled;
  child_pnp_power_callbacks.EvtDeviceD0Exit = XenPciPdo_EvtDeviceD0Exit;
  //child_pnp_power_callbacks.EvtDeviceD0ExitPreInterruptsDisabled = XenPciPdo_EvtDeviceD0ExitPreInterruptsDisabled;
  child_pnp_power_callbacks.EvtDevicePrepareHardware = XenPciPdo_EvtDevicePrepareHardware;
  child_pnp_power_callbacks.EvtDeviceReleaseHardware = XenPciPdo_EvtDeviceReleaseHardware;
  child_pnp_power_callbacks.EvtDeviceUsageNotification = XenPciPdo_EvtDeviceUsageNotification;
  WdfDeviceInitSetPnpPowerEventCallbacks(child_init, &child_pnp_power_callbacks);

  KdPrint((__DRIVER_NAME "     device = '%s', index = '%d', path = '%s'\n",
    identification->device, identification->index, identification->path));
  
  //status = WdfDeviceInitAssignWdmIrpPreprocessCallback(child_init, XenPciPdo_EvtDeviceWdmIrpPreprocess_START_DEVICE,
  //  IRP_MJ_PNP, pnp_minor_functions, ARRAY_SIZE(pnp_minor_functions));
  //if (!NT_SUCCESS(status)) {
  //  return status;
  //}
  
  //WDF_PDO_EVENT_CALLBACKS_INIT(&pdo_callbacks);
  //pdo_callbacks.EvtDeviceResourcesQuery = XenPciPdo_EvtDeviceResourcesQuery;
  //pdo_callbacks.EvtDeviceResourceRequirementsQuery = XenPciPdo_EvtDeviceResourceRequirementsQuery;
  //pdo_callbacks.EvtDeviceEject = XenPciPdo_EvtDeviceEject;
  //pdo_callbacks.EvtDeviceSetLock  = XenPciPdo_EvtDeviceSetLock;
  //WdfPdoInitSetEventCallbacks(child_init, &pdo_callbacks);

  RtlUnicodeStringPrintf(&buffer, L"xen\\%S", identification->device);
  status = WdfPdoInitAssignDeviceID(child_init, &buffer);
  if (!NT_SUCCESS(status))
  {
    return status;
  }
  status = WdfPdoInitAddHardwareID(child_init, &buffer);
  if (!NT_SUCCESS(status))
  {
    return status;
  }
  status = WdfPdoInitAddCompatibleID(child_init, &buffer);
  if (!NT_SUCCESS(status))
  {
    return status;
  }
  
  RtlUnicodeStringPrintf(&buffer, L"%02d", identification->index);
  status = WdfPdoInitAssignInstanceID(child_init, &buffer);
  if (!NT_SUCCESS(status))
  {
    return status;
  }
  
  RtlUnicodeStringPrintf(&buffer, L"Xen %S device #%d", identification->device, identification->index);
  status = WdfPdoInitAddDeviceText(child_init, &buffer, &location, 0x0409);
  if (!NT_SUCCESS(status))
  {
    return status;
  }
  WdfPdoInitSetDefaultLocale(child_init, 0x0409);

  WdfDeviceInitSetPowerNotPageable(child_init);

  WdfDeviceInitRegisterPnpStateChangeCallback(child_init, WdfDevStatePnpQueryCanceled, XenPci_EvtDevicePnpStateChange, StateNotificationEnterState);
  
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&child_attributes, XENPCI_PDO_DEVICE_DATA);
  status = WdfDeviceCreate(&child_init, &child_attributes, &child_device);
  if (!NT_SUCCESS(status))
  {
    return status;
  }

  xppdd = GetXppdd(child_device);
  
  xppdd->wdf_device = child_device;
  xppdd->wdf_device_bus_fdo = WdfChildListGetDevice(child_list);
  xppdd->xpdd = xpdd;

  xppdd->config_page_mdl = AllocateUncachedPage();

  WdfDeviceSetSpecialFileSupport(child_device, WdfSpecialFilePaging, TRUE);
  WdfDeviceSetSpecialFileSupport(child_device, WdfSpecialFileHibernation, TRUE);
  WdfDeviceSetSpecialFileSupport(child_device, WdfSpecialFileDump, TRUE);

  WDF_DEVICE_PNP_CAPABILITIES_INIT(&child_pnp_capabilities);
  child_pnp_capabilities.LockSupported = WdfFalse;
  child_pnp_capabilities.EjectSupported  = WdfTrue;
  child_pnp_capabilities.Removable  = WdfTrue;
  child_pnp_capabilities.DockDevice  = WdfFalse;
  child_pnp_capabilities.UniqueID  = WdfFalse;
  child_pnp_capabilities.SilentInstall  = WdfTrue;
  child_pnp_capabilities.SurpriseRemovalOK  = WdfTrue;
  child_pnp_capabilities.HardwareDisabled = WdfFalse;
  WdfDeviceSetPnpCapabilities(child_device, &child_pnp_capabilities);

  WDF_DEVICE_POWER_CAPABILITIES_INIT(&child_power_capabilities);
  child_power_capabilities.DeviceD1 = WdfTrue;
  child_power_capabilities.WakeFromD1 = WdfTrue;
  child_power_capabilities.DeviceWake = PowerDeviceD1;
  child_power_capabilities.DeviceState[PowerSystemWorking]   = PowerDeviceD0;
  child_power_capabilities.DeviceState[PowerSystemSleeping1] = PowerDeviceD1;
  child_power_capabilities.DeviceState[PowerSystemSleeping2] = PowerDeviceD2;
  child_power_capabilities.DeviceState[PowerSystemSleeping3] = PowerDeviceD2;
  child_power_capabilities.DeviceState[PowerSystemHibernate] = PowerDeviceD3;
  child_power_capabilities.DeviceState[PowerSystemShutdown]  = PowerDeviceD3;
  WdfDeviceSetPowerCapabilities(child_device, &child_power_capabilities);  

  RtlStringCbCopyA(xppdd->path, ARRAY_SIZE(xppdd->path), identification->path);
  RtlStringCbCopyA(xppdd->device, ARRAY_SIZE(xppdd->device), identification->device);
  xppdd->index = identification->index;
  KeInitializeEvent(&xppdd->backend_state_event, SynchronizationEvent, FALSE);
  ExInitializeFastMutex(&xppdd->backend_state_mutex);
  xppdd->backend_state = XenbusStateUnknown;
  xppdd->frontend_state = XenbusStateUnknown;
  xppdd->backend_path[0] = '\0';
  xppdd->backend_id = 0;
    
  FUNCTION_EXIT();
  
  return status;
}