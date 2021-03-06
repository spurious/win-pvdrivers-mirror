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

#define INITGUID
#include "xenpci.h"
#include <aux_klib.h>
#include <stdlib.h>

#pragma warning(disable : 4200) // zero-sized array

PMDL balloon_mdl_head = NULL;

/* Not really necessary but keeps PREfast happy */
DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD XenPci_EvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD XenPci_EvtDeviceAdd;
static EVT_WDF_DEVICE_USAGE_NOTIFICATION XenPci_EvtDeviceUsageNotification;
static EVT_WDF_DEVICE_PREPARE_HARDWARE XenHide_EvtDevicePrepareHardware;

#if (NTDDI_VERSION >= NTDDI_WS03SP1)
static KBUGCHECK_REASON_CALLBACK_ROUTINE XenPci_DebugHeaderDumpIoCallback;

/* this is supposed to be defined in wdm.h, but isn't */
NTSTATUS 
  KeInitializeCrashDumpHeader(
    IN ULONG  Type,
    IN ULONG  Flags,
    OUT PVOID  Buffer,
    IN ULONG  BufferSize,
    OUT PULONG  BufferNeeded OPTIONAL
    );
#endif

#define DUMP_TYPE_FULL 1

static VOID
XenPci_EvtDeviceUsageNotification(WDFDEVICE device, WDF_SPECIAL_FILE_TYPE notification_type, BOOLEAN is_in_notification_path)
{
  FUNCTION_ENTER();
  
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(is_in_notification_path);

  switch (notification_type)
  {
  case WdfSpecialFilePaging:
    FUNCTION_MSG("notification_type = Paging, flag = %d\n", is_in_notification_path);
    break;
  case WdfSpecialFileHibernation:
    FUNCTION_MSG("notification_type = Hibernation, flag = %d\n", is_in_notification_path);
    break;
  case WdfSpecialFileDump:
    FUNCTION_MSG("notification_type = Dump, flag = %d\n", is_in_notification_path);
    break;
  default:
    FUNCTION_MSG("notification_type = %d, flag = %d\n", notification_type, is_in_notification_path);
    break;
  }

  FUNCTION_EXIT();  
}

static NTSTATUS
XenPci_EvtDeviceAdd_XenPci(WDFDRIVER driver, PWDFDEVICE_INIT device_init)
{
  NTSTATUS status;
//  PDEVICE_OBJECT fdo = NULL;
//  PNP_BUS_INFORMATION busInfo;
//  DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\XenShutdown");
//  DECLARE_CONST_UNICODE_STRING(SymbolicName, L"\\DosDevices\\XenShutdown");
  WDF_CHILD_LIST_CONFIG child_list_config;
  WDFDEVICE device;
  PXENPCI_DEVICE_DATA xpdd;
  UNICODE_STRING reference;
  WDF_OBJECT_ATTRIBUTES device_attributes;
  PNP_BUS_INFORMATION pbi;
  WDF_PNPPOWER_EVENT_CALLBACKS pnp_power_callbacks;
  WDF_INTERRUPT_CONFIG interrupt_config;
  WDF_OBJECT_ATTRIBUTES file_attributes;
  WDF_FILEOBJECT_CONFIG file_config;
  WDF_IO_QUEUE_CONFIG queue_config;
  WDFCOLLECTION veto_devices;
  WDFKEY param_key;
  DECLARE_CONST_UNICODE_STRING(veto_devices_name, L"veto_devices");
  WDF_DEVICE_POWER_CAPABILITIES power_capabilities;
  PPHYSICAL_MEMORY_RANGE pmr_head, pmr;
  int i;
  
  UNREFERENCED_PARAMETER(driver);

  FUNCTION_ENTER();

  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power_callbacks);
  pnp_power_callbacks.EvtDeviceD0Entry = XenPci_EvtDeviceD0Entry;
  pnp_power_callbacks.EvtDeviceD0EntryPostInterruptsEnabled = XenPci_EvtDeviceD0EntryPostInterruptsEnabled;
  pnp_power_callbacks.EvtDeviceD0Exit = XenPci_EvtDeviceD0Exit;
  pnp_power_callbacks.EvtDeviceD0ExitPreInterruptsDisabled = XenPci_EvtDeviceD0ExitPreInterruptsDisabled;
  pnp_power_callbacks.EvtDevicePrepareHardware = XenPci_EvtDevicePrepareHardware;
  pnp_power_callbacks.EvtDeviceReleaseHardware = XenPci_EvtDeviceReleaseHardware;
  pnp_power_callbacks.EvtDeviceQueryRemove = XenPci_EvtDeviceQueryRemove;
  pnp_power_callbacks.EvtDeviceUsageNotification = XenPci_EvtDeviceUsageNotification;

  WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp_power_callbacks);

  WdfDeviceInitSetDeviceType(device_init, FILE_DEVICE_BUS_EXTENDER);
  WdfDeviceInitSetExclusive(device_init, FALSE);

  WDF_CHILD_LIST_CONFIG_INIT(&child_list_config, sizeof(XENPCI_PDO_IDENTIFICATION_DESCRIPTION), XenPci_EvtChildListCreateDevice);
  child_list_config.EvtChildListScanForChildren = XenPci_EvtChildListScanForChildren;
  WdfFdoInitSetDefaultChildListConfig(device_init, &child_list_config, WDF_NO_OBJECT_ATTRIBUTES);

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&file_attributes, XENPCI_DEVICE_INTERFACE_DATA);
  WDF_FILEOBJECT_CONFIG_INIT(&file_config, XenPci_EvtDeviceFileCreate, XenPci_EvtFileClose, XenPci_EvtFileCleanup);
  WdfDeviceInitSetFileObjectConfig(device_init, &file_config, &file_attributes);
  
  WdfDeviceInitSetIoType(device_init, WdfDeviceIoBuffered);

  WdfDeviceInitSetPowerNotPageable(device_init);
  
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, XENPCI_DEVICE_DATA);
  status = WdfDeviceCreate(&device_init, &device_attributes, &device);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("Error creating device %08x\n", status);
    return status;
  }

  xpdd = GetXpdd(device);
  xpdd->wdf_device = device;
  xpdd->child_list = WdfFdoGetDefaultChildList(device);

  /* this is not a documented function */
  KeInitializeEvent(&xpdd->balloon_event, SynchronizationEvent, FALSE);
  pmr_head = MmGetPhysicalMemoryRanges();
  xpdd->current_memory_kb = 0;
  for (pmr = pmr_head; !(pmr->BaseAddress.QuadPart == 0 && pmr->NumberOfBytes.QuadPart == 0); pmr++) {
    xpdd->current_memory_kb += (ULONG)(pmr->NumberOfBytes.QuadPart / 1024);
  }
  FUNCTION_MSG("current_memory_kb = %d\n", xpdd->current_memory_kb);
  /* round to MB increments because that is what balloon deals in */
  xpdd->current_memory_kb = (xpdd->current_memory_kb + 0x1FF) & 0xFFFFFC00;
  FUNCTION_MSG("current_memory_kb rounded to %d\n", xpdd->current_memory_kb);

  ExInitializeFastMutex(&xpdd->suspend_mutex);
  WdfCollectionCreate(WDF_NO_OBJECT_ATTRIBUTES, &veto_devices);
  status = WdfDriverOpenParametersRegistryKey(driver, KEY_QUERY_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &param_key);
  if (NT_SUCCESS(status)) {
    status = WdfRegistryQueryMultiString(param_key, &veto_devices_name, WDF_NO_OBJECT_ATTRIBUTES, veto_devices);
    if (!NT_SUCCESS(status)) {
      FUNCTION_MSG("Error reading parameters/veto_devices value %08x\n", status);
    }
    WdfRegistryClose(param_key);
  } else {
    FUNCTION_MSG("Error opening parameters key %08x\n", status);
  }

  InitializeListHead(&xpdd->veto_list);
  for (i = 0; i < (int)WdfCollectionGetCount(veto_devices); i++) {
    WDFOBJECT ws;
    UNICODE_STRING val;
    ANSI_STRING s;
    PVOID entry;
    ws = WdfCollectionGetItem(veto_devices, i);
    WdfStringGetUnicodeString(ws, &val);
    RtlUnicodeStringToAnsiString(&s, &val, TRUE);
    entry = ExAllocatePoolWithTag(NonPagedPool, sizeof(LIST_ENTRY) + s.Length + 1, XENPCI_POOL_TAG);
    memcpy((PUCHAR)entry + sizeof(LIST_ENTRY), s.Buffer, s.Length + 1);
    RtlFreeAnsiString(&s);
    InsertTailList(&xpdd->veto_list, (PLIST_ENTRY)entry);
  }
  WDF_DEVICE_POWER_CAPABILITIES_INIT(&power_capabilities);
  power_capabilities.DeviceD1 = WdfTrue;
  power_capabilities.WakeFromD1 = WdfTrue;
  power_capabilities.DeviceWake = PowerDeviceD1;
  power_capabilities.DeviceState[PowerSystemWorking]   = PowerDeviceD0;
  power_capabilities.DeviceState[PowerSystemSleeping1] = PowerDeviceD1;
  power_capabilities.DeviceState[PowerSystemSleeping2] = PowerDeviceD2;
  power_capabilities.DeviceState[PowerSystemSleeping3] = PowerDeviceD2;
  power_capabilities.DeviceState[PowerSystemHibernate] = PowerDeviceD3;
  power_capabilities.DeviceState[PowerSystemShutdown]  = PowerDeviceD3;
  WdfDeviceSetPowerCapabilities(device, &power_capabilities);  

  WdfDeviceSetSpecialFileSupport(device, WdfSpecialFilePaging, TRUE);
  WdfDeviceSetSpecialFileSupport(device, WdfSpecialFileHibernation, TRUE);
  WdfDeviceSetSpecialFileSupport(device, WdfSpecialFileDump, TRUE);

  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config, WdfIoQueueDispatchParallel);
  queue_config.EvtIoDefault = XenPci_EvtIoDefault;
  status = WdfIoQueueCreate(device, &queue_config, WDF_NO_OBJECT_ATTRIBUTES, &xpdd->io_queue);
  if (!NT_SUCCESS(status)) {
      FUNCTION_MSG("Error creating queue 0x%x\n", status);
      return status;
  }
  
  WDF_INTERRUPT_CONFIG_INIT(&interrupt_config, EvtChn_EvtInterruptIsr, NULL);
  interrupt_config.EvtInterruptEnable  = EvtChn_EvtInterruptEnable;
  interrupt_config.EvtInterruptDisable = EvtChn_EvtInterruptDisable;

  status = WdfInterruptCreate(device, &interrupt_config, WDF_NO_OBJECT_ATTRIBUTES, &xpdd->interrupt);
  if (!NT_SUCCESS(status))
  {
    FUNCTION_MSG("Error creating interrupt 0x%x\n", status);
    return status;
  }
  
  RtlInitUnicodeString(&reference, L"xenbus");
  status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_XENBUS, &reference);
  if (!NT_SUCCESS(status)) {
      FUNCTION_MSG("Error registering device interface 0x%x\n", status);
      return status;
  }

  RtlInitUnicodeString(&reference, L"evtchn");
  status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_EVTCHN, &reference);
  if (!NT_SUCCESS(status)) {
      FUNCTION_MSG("Error registering device interface 0x%x\n", status);
      return status;
  }

  RtlInitUnicodeString(&reference, L"gntdev");
  status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_GNTDEV, &reference);
  if (!NT_SUCCESS(status)) {
      FUNCTION_MSG("Error registering device interface 0x%x\n", status);
      return status;
  }

  pbi.BusTypeGuid = GUID_BUS_TYPE_XEN;
  pbi.LegacyBusType = PNPBus;
  pbi.BusNumber = 0;
  WdfDeviceSetBusInformationForChildren(device, &pbi);

  xpdd->removable = TRUE;

  FUNCTION_EXIT();
  return status;
}

NTSTATUS
XenHide_EvtDevicePrepareHardware(WDFDEVICE device, WDFCMRESLIST resources_raw, WDFCMRESLIST resources_translated)
{
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(resources_raw);
  UNREFERENCED_PARAMETER(resources_translated);
  FUNCTION_ENTER();
  FUNCTION_EXIT();
  return STATUS_UNSUCCESSFUL;
}

static BOOLEAN
XenPci_IdSuffixMatches(PWDFDEVICE_INIT device_init, PWCHAR matching_id) {
  NTSTATUS status;
  WDFMEMORY memory;
  ULONG remaining;
  size_t string_length;
  PWCHAR ids;
  PWCHAR ptr;
  size_t ids_length;
  ULONG properties[] = {DevicePropertyCompatibleIDs, DevicePropertyHardwareID};
  int i;
  
//  FUNCTION_ENTER();
  for (i = 0; i < ARRAY_SIZE(properties); i++)
  {

    status = WdfFdoInitAllocAndQueryProperty(device_init, properties[i], NonPagedPool, WDF_NO_OBJECT_ATTRIBUTES, &memory);
    if (!NT_SUCCESS(status))
      continue;
    ids = WdfMemoryGetBuffer(memory, &ids_length);

    if (!NT_SUCCESS(status)) {
      continue;
    }
    
    remaining = (ULONG)ids_length / 2;
    for (ptr = ids; *ptr != 0; ptr += string_length + 1) {
      RtlStringCchLengthW(ptr, remaining, &string_length);
      remaining -= (ULONG)string_length + 1;
      if (string_length >= wcslen(matching_id)) {
        ptr += string_length - wcslen(matching_id);
        string_length = wcslen(matching_id);
      }
      if (wcscmp(ptr, matching_id) == 0) {
        WdfObjectDelete(memory);
        return TRUE;
      }
    }
    WdfObjectDelete(memory);
  }
  return FALSE;
}

WDFCOLLECTION qemu_hide_devices;
USHORT qemu_hide_flags_value;

static NTSTATUS
XenPci_EvtDeviceAdd_XenHide(WDFDRIVER driver, PWDFDEVICE_INIT device_init)
{
  NTSTATUS status;
  WDFMEMORY memory;
  PWCHAR device_description;
  WDF_PNPPOWER_EVENT_CALLBACKS pnp_power_callbacks;
  WDF_OBJECT_ATTRIBUTES device_attributes;
  BOOLEAN hide_required = FALSE;
  WDFDEVICE device;
  ULONG i;

  UNREFERENCED_PARAMETER(driver);

  FUNCTION_ENTER();

  status = WdfFdoInitAllocAndQueryProperty(device_init, DevicePropertyDeviceDescription, NonPagedPool, WDF_NO_OBJECT_ATTRIBUTES, &memory);
  if (NT_SUCCESS(status)) {
    device_description = WdfMemoryGetBuffer(memory, NULL);
  } else {
    device_description = L"<unknown device>";
  }
  
  for (i = 0; i < WdfCollectionGetCount(qemu_hide_devices); i++) {
    WDFSTRING wdf_string = WdfCollectionGetItem(qemu_hide_devices, i);
    UNICODE_STRING unicode_string;
    WdfStringGetUnicodeString(wdf_string, &unicode_string);
    if (XenPci_IdSuffixMatches(device_init, unicode_string.Buffer)) {
      hide_required = TRUE;
      break;
    }
  }
  if (!hide_required) {
    FUNCTION_MSG("(filter not required for %S)\n", device_description);
    WdfObjectDelete(memory);
    return STATUS_SUCCESS;
  }
  
  FUNCTION_MSG("Installing Filter for %S\n", device_description);
  
  WdfFdoInitSetFilter(device_init);
  WdfDeviceInitSetDeviceType(device_init, FILE_DEVICE_UNKNOWN);
  WdfDeviceInitSetExclusive(device_init, FALSE);

  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power_callbacks);
  pnp_power_callbacks.EvtDevicePrepareHardware = XenHide_EvtDevicePrepareHardware;
  WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp_power_callbacks);
  
  WDF_OBJECT_ATTRIBUTES_INIT(&device_attributes);
  status = WdfDeviceCreate(&device_init, &device_attributes, &device);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("Error creating device %08x\n", status);
    WdfObjectDelete(memory);
    FUNCTION_EXIT();
    return status;
  }

  WdfObjectDelete(memory);
  FUNCTION_EXIT();

  return status;
}

static NTSTATUS
XenPci_EvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT device_init)
{
  if (!hypercall_stubs) {
    return STATUS_SUCCESS;
  } else if (XenPci_IdSuffixMatches(device_init, L"VEN_5853&DEV_0001")) {
    FUNCTION_MSG("Xen PCI device found - must be fdo\n");
    return XenPci_EvtDeviceAdd_XenPci(driver, device_init);
  } else if (WdfCollectionGetCount(qemu_hide_devices) > 0) {
    FUNCTION_MSG("Xen PCI device not found - must be filter\n");
    return XenPci_EvtDeviceAdd_XenHide(driver, device_init);  
  } else {
    return STATUS_SUCCESS;
  }
}

ULONG qemu_protocol_version;
ULONG tpr_patch_requested;
extern PULONG InitSafeBootMode;

VOID
XenPci_HideQemuDevices() {
  #pragma warning(suppress:28138)
  WRITE_PORT_USHORT(XEN_IOPORT_DEVICE_MASK, (USHORT)qemu_hide_flags_value); //QEMU_UNPLUG_ALL_IDE_DISKS|QEMU_UNPLUG_ALL_NICS);
  XnDebugPrint("disabled qemu devices %02x\n", qemu_hide_flags_value);
}

static BOOLEAN
XenPci_CheckHideQemuDevices() {
  #pragma warning(suppress:28138)
  if (READ_PORT_USHORT(XEN_IOPORT_MAGIC) == 0x49d2) {
    #pragma warning(suppress:28138)
    qemu_protocol_version = READ_PORT_UCHAR(XEN_IOPORT_VERSION);
    XnDebugPrint("qemu version = %d\n", qemu_protocol_version);
    switch(qemu_protocol_version) {
    case 1:
      #pragma warning(suppress:28138)
      WRITE_PORT_USHORT(XEN_IOPORT_PRODUCT, XEN_PV_PRODUCT_NUMBER);
      #pragma warning(suppress:28138)
      WRITE_PORT_ULONG(XEN_IOPORT_BUILD, XEN_PV_PRODUCT_BUILD);
      #pragma warning(suppress:28138)
      if (READ_PORT_USHORT(XEN_IOPORT_MAGIC) != 0x49d2)
      {
        XnDebugPrint("qemu we are blacklisted\n");
        break;
      }
      /* fall through */
    case 0:
      return TRUE;
    default:
      XnDebugPrint("unknown qemu version %d\n", qemu_protocol_version);
      break;
    }
  }
  return FALSE;
}

/*
make sure the load order is System Reserved, Dummy Group, WdfLoadGroup, XenPCI, Boot Bus Extender
*/

static VOID
XenPci_FixLoadOrder()
{
  NTSTATUS status;
  WDFCOLLECTION old_load_order, new_load_order;
  DECLARE_CONST_UNICODE_STRING(sgo_name, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\ServiceGroupOrder");
  DECLARE_CONST_UNICODE_STRING(list_name, L"List");
  WDFKEY sgo_key;
  ULONG i;
  LONG dummy_group_index = -1;
  LONG boot_bus_extender_index = -1;
  LONG xenpci_group_index = -1;
  LONG wdf_load_group_index = -1;
  DECLARE_CONST_UNICODE_STRING(dummy_group_name, L"Dummy Group");
  DECLARE_CONST_UNICODE_STRING(wdf_load_group_name, L"WdfLoadGroup");
  DECLARE_CONST_UNICODE_STRING(xenpci_group_name, L"XenPCI Group");
  DECLARE_CONST_UNICODE_STRING(boot_bus_extender_name, L"Boot Bus Extender");

  FUNCTION_ENTER();
  
  status = WdfRegistryOpenKey(NULL, &sgo_name, KEY_QUERY_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &sgo_key);
  if (!NT_SUCCESS(status))
  {
    FUNCTION_MSG("Error opening ServiceGroupOrder key %08x\n", status);
    return;
  }
  WdfCollectionCreate(WDF_NO_OBJECT_ATTRIBUTES, &old_load_order);
  WdfCollectionCreate(WDF_NO_OBJECT_ATTRIBUTES, &new_load_order);  
  status = WdfRegistryQueryMultiString(sgo_key, &list_name, WDF_NO_OBJECT_ATTRIBUTES, old_load_order);
  if (!NT_SUCCESS(status))
  {
    FUNCTION_MSG("Error reading ServiceGroupOrder\\List value %08x\n", status);
    WdfObjectDelete(new_load_order);
    WdfObjectDelete(old_load_order);
    return;
  }
  //FUNCTION_MSG("Current Order:\n");        
  for (i = 0; i < WdfCollectionGetCount(old_load_order); i++)
  {
    WDFOBJECT ws = WdfCollectionGetItem(old_load_order, i);
    UNICODE_STRING val;
    WdfStringGetUnicodeString(ws, &val);
    if (!RtlCompareUnicodeString(&val, &dummy_group_name, TRUE))
      dummy_group_index = (ULONG)i;
    if (!RtlCompareUnicodeString(&val, &wdf_load_group_name, TRUE))
      wdf_load_group_index = (ULONG)i;         
    if (!RtlCompareUnicodeString(&val, &xenpci_group_name, TRUE))
      xenpci_group_index = (ULONG)i;         
    if (!RtlCompareUnicodeString(&val, &boot_bus_extender_name, TRUE))
      boot_bus_extender_index = (ULONG)i;         
    //FUNCTION_MSG("  %wZ\n", &val);        
  }
  FUNCTION_MSG("dummy_group_index = %d\n", dummy_group_index);
  FUNCTION_MSG("wdf_load_group_index = %d\n", wdf_load_group_index);
  FUNCTION_MSG("xenpci_group_index = %d\n", xenpci_group_index);
  FUNCTION_MSG("boot_bus_extender_index = %d\n", boot_bus_extender_index);
  if (boot_bus_extender_index == -1)
  {
    WdfObjectDelete(new_load_order);
    WdfObjectDelete(old_load_order);
    WdfRegistryClose(sgo_key);
    return; /* something is very wrong */
  }
  if (dummy_group_index == 1 && wdf_load_group_index != -1 &&
    (dummy_group_index < wdf_load_group_index
    && wdf_load_group_index < xenpci_group_index
    && xenpci_group_index < boot_bus_extender_index))
  {
    FUNCTION_EXIT();
    return; /* our work here is done */
  }
  for (i = 0; i < WdfCollectionGetCount(old_load_order); i++)
  {
    WDFOBJECT ws;
    if (i == 1)
    {
      WDFSTRING tmp_wdf_string;
      WdfStringCreate(&dummy_group_name, WDF_NO_OBJECT_ATTRIBUTES, &tmp_wdf_string);
      WdfCollectionAdd(new_load_order, tmp_wdf_string);
      WdfObjectDelete(tmp_wdf_string);
    }
    if (i == 1)
    {
      WDFSTRING tmp_wdf_string;
      WdfStringCreate(&wdf_load_group_name, WDF_NO_OBJECT_ATTRIBUTES, &tmp_wdf_string);
      WdfCollectionAdd(new_load_order, tmp_wdf_string);
      WdfObjectDelete(tmp_wdf_string);
    }
    if (i == 1)
    {
      WDFSTRING tmp_wdf_string;
      WdfStringCreate(&xenpci_group_name, WDF_NO_OBJECT_ATTRIBUTES, &tmp_wdf_string);
      WdfCollectionAdd(new_load_order, tmp_wdf_string);
      WdfObjectDelete(tmp_wdf_string);
    }
    if (i == (ULONG)dummy_group_index || i == (ULONG)wdf_load_group_index || i == (ULONG)xenpci_group_index)
      continue;
    ws = WdfCollectionGetItem(old_load_order, i);
    WdfCollectionAdd(new_load_order, ws);
  }
  WdfRegistryAssignMultiString(sgo_key, &list_name, new_load_order);
  //FUNCTION_MSG("New Order:\n");        
  for (i = 0; i < WdfCollectionGetCount(new_load_order); i++)
  {
    WDFOBJECT ws = WdfCollectionGetItem(new_load_order, i);
    UNICODE_STRING val;
    WdfStringGetUnicodeString(ws, &val);
    //FUNCTION_MSG("  %wZ\n", &val);        
  }
  WdfObjectDelete(new_load_order);
  WdfObjectDelete(old_load_order);
  WdfRegistryClose(sgo_key);
  
  FUNCTION_EXIT();
  
  return;
}

#if (NTDDI_VERSION >= NTDDI_WS03SP1)  
/* this isn't freed on shutdown... perhaps it should be */
static PUCHAR dump_header = NULL;
static ULONG dump_header_size;
static KBUGCHECK_REASON_CALLBACK_RECORD callback_record;
static ULONG64 dump_current_offset = 0;
#define DUMP_HEADER_PREFIX_SIZE 8
#define DUMP_HEADER_SUFFIX_SIZE 8

/* call KeInitializeCrashDumpHeader once on crash */
static VOID
XenPci_DebugHeaderDumpIoCallback(
  KBUGCHECK_CALLBACK_REASON reason,
  PKBUGCHECK_REASON_CALLBACK_RECORD record,
  PVOID reason_specific_data,
  ULONG reason_specific_data_length) {
  UNREFERENCED_PARAMETER(record);
  UNREFERENCED_PARAMETER(reason_specific_data);
  UNREFERENCED_PARAMETER(reason_specific_data_length);
  
  if (dump_header && reason == KbCallbackDumpIo) {
    PKBUGCHECK_DUMP_IO dump_io = reason_specific_data;
    if (dump_io->Type == KbDumpIoHeader ) {
      if (dump_io->Offset != -1) {
        dump_current_offset = dump_io->Offset;
      }
      XN_ASSERT(dump_current_offset + dump_io->BufferLength <= dump_header_size);
      RtlCopyMemory(dump_header + DUMP_HEADER_PREFIX_SIZE + dump_current_offset, dump_io->Buffer, dump_io->BufferLength);
      dump_current_offset += dump_io->BufferLength;
    } else if (dump_io->Type == KbDumpIoComplete) {
      dump_current_offset = 0;
    }
  }
}
#endif

#define XEN_SIGNATURE_LOWER 0x40000000
#define XEN_SIGNATURE_UPPER 0x4000FFFF

USHORT xen_version_major = (USHORT)-1;
USHORT xen_version_minor = (USHORT)-1;
PVOID hypercall_stubs = NULL;

static VOID
XenPCI_GetHypercallStubs() {
  ULONG base;
  DWORD32 cpuid_output[4];
  char xensig[13];
  ULONG i;
  ULONG pages;
  ULONG msr;

  if (hypercall_stubs) {
    FUNCTION_MSG("hypercall_stubs already set\n");
    return;
  }

  for (base = XEN_SIGNATURE_LOWER; base < XEN_SIGNATURE_UPPER; base += 0x100) {
    __cpuid(cpuid_output, base);
    *(ULONG*)(xensig + 0) = cpuid_output[1];
    *(ULONG*)(xensig + 4) = cpuid_output[2];
    *(ULONG*)(xensig + 8) = cpuid_output[3];
    xensig[12] = '\0';
    FUNCTION_MSG("base = 0x%08x, Xen Signature = %s, EAX = 0x%08x\n", base, xensig, cpuid_output[0]);
    if (!strncmp("XenVMMXenVMM", xensig, 12) && ((cpuid_output[0] - base) >= 2))
      break;
  }
  if (base >= XEN_SIGNATURE_UPPER) {
    FUNCTION_MSG("Cannot find Xen signature\n");
    return;
  }

  __cpuid(cpuid_output, base + 1);
  xen_version_major = (USHORT)(cpuid_output[0] >> 16);
  xen_version_minor = (USHORT)(cpuid_output[0] & 0xFFFF);
  FUNCTION_MSG("Xen Version %d.%d\n", xen_version_major, xen_version_minor);

  __cpuid(cpuid_output, base + 2);
  pages = cpuid_output[0];
  msr = cpuid_output[1];

  hypercall_stubs = ExAllocatePoolWithTag(NonPagedPool, pages * PAGE_SIZE, XENPCI_POOL_TAG);
  FUNCTION_MSG("Hypercall area at %p\n", hypercall_stubs);

  if (!hypercall_stubs)
    return;
  for (i = 0; i < pages; i++) {
    ULONGLONG pfn;
    pfn = (MmGetPhysicalAddress((PUCHAR)hypercall_stubs + i * PAGE_SIZE).QuadPart >> PAGE_SHIFT);
    __writemsr(msr, (pfn << PAGE_SHIFT) + i);
  }
}

static VOID
XenPCI_FreeHypercallStubs() {
  if (hypercall_stubs) {
    ExFreePoolWithTag(hypercall_stubs, XENPCI_POOL_TAG);
  }
  hypercall_stubs = NULL;
}

VOID
XenPci_EvtDriverUnload(WDFDRIVER driver) {
  UNREFERENCED_PARAMETER(driver);

  FUNCTION_ENTER();
  
#if (NTDDI_VERSION >= NTDDI_WS03SP1)
  KeDeregisterBugCheckReasonCallback(&callback_record);
  if (dump_header) {
    MmFreeContiguousMemory(dump_header);
  }
#endif
  FUNCTION_EXIT();
}

  
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
  NTSTATUS status = STATUS_SUCCESS;
  WDF_DRIVER_CONFIG config;
  WDFDRIVER driver;
  WDF_OBJECT_ATTRIBUTES parent_attributes;
  PCONFIGURATION_INFORMATION conf_info;
  WDFKEY control_key;
  WDFKEY param_key;
  ULONG always_patch = 0;
  ULONG always_hide = 0;
  DECLARE_CONST_UNICODE_STRING(control_key_name, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control");
  DECLARE_CONST_UNICODE_STRING(system_start_options_name, L"SystemStartOptions");
  DECLARE_CONST_UNICODE_STRING(txt_always_hide_name, L"txt_hide_qemu_always");
  DECLARE_CONST_UNICODE_STRING(hide_devices_name, L"hide_devices");
  DECLARE_CONST_UNICODE_STRING(txt_always_patch_name, L"txt_patch_tpr_always");
  WDFSTRING wdf_system_start_options;
  UNICODE_STRING system_start_options;
#if (NTDDI_VERSION >= NTDDI_WS03SP1)
  PHYSICAL_ADDRESS dump_header_mem_max;
#endif
  
  UNREFERENCED_PARAMETER(RegistryPath);

  FUNCTION_ENTER();

  FUNCTION_MSG(__DRIVER_NAME " " VER_FILEVERSION_STR "\n");

  XenPCI_GetHypercallStubs();
  
#if (NTDDI_VERSION >= NTDDI_WS03SP1)
  if (hypercall_stubs) {
    status = KeInitializeCrashDumpHeader(DUMP_TYPE_FULL, 0, NULL, 0, &dump_header_size);
    /* try and allocate contiguous memory as low as possible */
    dump_header_mem_max.QuadPart = 0xFFFFF;
    while (!dump_header && dump_header_mem_max.QuadPart != 0xFFFFFFFFFFFFFFFF) {
      dump_header = MmAllocateContiguousMemory(DUMP_HEADER_PREFIX_SIZE + dump_header_size + DUMP_HEADER_SUFFIX_SIZE, dump_header_mem_max);
      if (dump_header) {
        FUNCTION_MSG("Allocated crash dump header < 0x%016I64x\n", dump_header_mem_max.QuadPart);
        break;
      }
      dump_header_mem_max.QuadPart = (dump_header_mem_max.QuadPart << 4) | 0xF;
    }
    if (dump_header) {
      status = KeInitializeCrashDumpHeader(DUMP_TYPE_FULL, 0, dump_header + DUMP_HEADER_PREFIX_SIZE, dump_header_size, &dump_header_size);
      FUNCTION_MSG("KeInitializeCrashDumpHeader status = %08x, size = %d\n", status, dump_header_size);
      memcpy(dump_header + 0, "XENXEN", 6); /* magic number */
      *(PUSHORT)(dump_header + 6) = (USHORT)(INT_PTR)dump_header & (PAGE_SIZE - 1); /* store offset too as additional verification */
      memcpy(dump_header + DUMP_HEADER_PREFIX_SIZE + dump_header_size, "XENXEN", 6);
      *(PUSHORT)(dump_header + DUMP_HEADER_PREFIX_SIZE + dump_header_size + 6) = (USHORT)(INT_PTR)dump_header & (PAGE_SIZE - 1); /* store offset too as additional verification */
      KeInitializeCallbackRecord(&callback_record);
      KeRegisterBugCheckReasonCallback(&callback_record, XenPci_DebugHeaderDumpIoCallback, KbCallbackDumpIo, (PUCHAR)"XenPci_DebugHeaderDumpIoCallback");
    } else {
      FUNCTION_MSG("Failed to allocate memory for crash dump header\n");
    }
  }
#endif
  WDF_DRIVER_CONFIG_INIT(&config, XenPci_EvtDeviceAdd);
  config.EvtDriverUnload = XenPci_EvtDriverUnload;
  status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("WdfDriverCreate failed with status 0x%x\n", status);
    FUNCTION_EXIT();
    return status;
  }
  if (hypercall_stubs) {
    WDF_OBJECT_ATTRIBUTES_INIT(&parent_attributes);
    parent_attributes.ParentObject = driver;
    
    status = WdfDriverOpenParametersRegistryKey(driver, KEY_QUERY_VALUE, &parent_attributes, &param_key);
    if (!NT_SUCCESS(status)) {
      FUNCTION_MSG("Error opening parameters key %08x\n", status);
      goto error;
    }

    status = AuxKlibInitialize();
    if(!NT_SUCCESS(status)) {
      FUNCTION_MSG("AuxKlibInitialize failed %08x\n", status);
      goto error;
    }

    XenPci_FixLoadOrder();

    RtlInitUnicodeString(&system_start_options, L"failed to read");
    status = WdfRegistryOpenKey(NULL, &control_key_name, GENERIC_READ, &parent_attributes, &control_key);
    if (NT_SUCCESS(status)) {
      status = WdfStringCreate(NULL, &parent_attributes, &wdf_system_start_options);
      status = WdfRegistryQueryString(control_key, &system_start_options_name, wdf_system_start_options);
      if (NT_SUCCESS(status))
        WdfStringGetUnicodeString(wdf_system_start_options, &system_start_options);
    }
    WdfRegistryClose(control_key);

    FUNCTION_MSG("SystemStartOptions = %wZ\n", &system_start_options);
    
    always_patch = 0;
    WdfRegistryQueryULong(param_key, &txt_always_patch_name, &always_patch);
    if (always_patch || (system_start_options.Buffer && wcsstr(system_start_options.Buffer, L"PATCHTPR"))) {
      DECLARE_CONST_UNICODE_STRING(verifier_key_name, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Session Manager\\Memory Management");
      WDFKEY memory_key;
      ULONG verifier_value;
      
      FUNCTION_MSG("PATCHTPR found\n");
      
      tpr_patch_requested = TRUE;
      status = WdfRegistryOpenKey(NULL, &verifier_key_name, KEY_READ, &parent_attributes, &memory_key);
      if (NT_SUCCESS(status))
      {
        DECLARE_CONST_UNICODE_STRING(verifier_value_name, L"VerifyDriverLevel");
        status = WdfRegistryQueryULong(memory_key, &verifier_value_name, &verifier_value);
        if (NT_SUCCESS(status) && verifier_value != 0)
        {
          FUNCTION_MSG("Verifier active - not patching\n");
          tpr_patch_requested = FALSE;
        }
        WdfRegistryClose(memory_key);
      }
    }

    WdfCollectionCreate(&parent_attributes, &qemu_hide_devices);
    WdfRegistryQueryULong(param_key, &txt_always_hide_name, &always_hide);
    conf_info = IoGetConfigurationInformation();      
    if (always_hide || ((conf_info == NULL || conf_info->DiskCount == 0)
        && !(system_start_options.Buffer && wcsstr(system_start_options.Buffer, L"NOGPLPV"))
        && !(system_start_options.Buffer && wcsstr(system_start_options.Buffer, L"NOEJBPV"))
        && !*InitSafeBootMode)) {
      if (!(system_start_options.Buffer && wcsstr(system_start_options.Buffer, L"EJBPVUSEFILTERHIDE")) && XenPci_CheckHideQemuDevices()) {
        DECLARE_CONST_UNICODE_STRING(qemu_hide_flags_name, L"qemu_hide_flags");
        DECLARE_CONST_UNICODE_STRING(txt_qemu_hide_flags_name, L"txt_qemu_hide_flags");
        WDFCOLLECTION qemu_hide_flags;
        ULONG i;

        WdfCollectionCreate(&parent_attributes, &qemu_hide_flags);
        WdfRegistryQueryMultiString(param_key, &qemu_hide_flags_name, &parent_attributes, qemu_hide_flags);
        WdfRegistryQueryMultiString(param_key, &txt_qemu_hide_flags_name, &parent_attributes, qemu_hide_flags);
        for (i = 0; i < WdfCollectionGetCount(qemu_hide_flags); i++) {
          ULONG value;
          WDFSTRING wdf_string = WdfCollectionGetItem(qemu_hide_flags, i);
          UNICODE_STRING unicode_string;
          WdfStringGetUnicodeString(wdf_string, &unicode_string);
          status = RtlUnicodeStringToInteger(&unicode_string, 0, &value);
          qemu_hide_flags_value |= value;
        }
        WdfObjectDelete(qemu_hide_flags);
        XenPci_HideQemuDevices();
      } else {
        WdfRegistryQueryMultiString(param_key, &hide_devices_name, &parent_attributes, qemu_hide_devices);      
      }
    }
    WdfRegistryClose(param_key);
  }
  FUNCTION_EXIT();
  return STATUS_SUCCESS;

error:
  FUNCTION_MSG("Failed, returning %08x\n", status);
  FUNCTION_EXIT();
  return status;
}
