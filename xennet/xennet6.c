/*
PV Net Driver for Windows Xen HVM Domains
Copyright (C) 2007 James Harper
Copyright (C) 2007 Andrew Grover <andy.grover@oracle.com>

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

#include <stdlib.h>
#include <io/xenbus.h>
#include "xennet6.h"

/* Not really necessary but keeps PREfast happy */
DRIVER_INITIALIZE DriverEntry;
static IO_WORKITEM_ROUTINE XenNet_ResumeWorkItem;
#if (VER_PRODUCTBUILD >= 7600)
//static KDEFERRED_ROUTINE XenNet_SuspendResume;
static KDEFERRED_ROUTINE XenNet_RxTxDpc;
#endif
static VOID XenNet_DeviceCallback(PVOID context, ULONG callback_type, PVOID value);

#pragma NDIS_INIT_FUNCTION(DriverEntry)

NDIS_HANDLE driver_handle = NULL;

USHORT ndis_os_major_version = 0;
USHORT ndis_os_minor_version = 0;

static VOID
XenNet_RxTxDpc(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
  struct xennet_info *xi = context;
  BOOLEAN dont_set_event;

  UNREFERENCED_PARAMETER(dpc);
  UNREFERENCED_PARAMETER(arg1);
  UNREFERENCED_PARAMETER(arg2);

  //FUNCTION_ENTER();
  /* if Rx goes over its per-dpc quota then make sure TxBufferGC doesn't set an event as we are already guaranteed to be called again */
  dont_set_event = XenNet_RxBufferCheck(xi);
  XenNet_TxBufferGC(xi, dont_set_event);
  //FUNCTION_EXIT();
} 

static BOOLEAN
XenNet_HandleEvent_DIRQL(PVOID context)
{
  struct xennet_info *xi = context;
  //ULONG suspend_resume_state_pdo;
  
  //FUNCTION_ENTER();
  if (xi->device_state == DEVICE_STATE_ACTIVE || xi->device_state == DEVICE_STATE_DISCONNECTING) {
    KeInsertQueueDpc(&xi->rxtx_dpc, NULL, NULL);
  }
  //FUNCTION_EXIT();
  return TRUE;
}

static NTSTATUS
XenNet_Connect(PVOID context, BOOLEAN suspend) {
  NTSTATUS status;
  struct xennet_info *xi = context;
  PFN_NUMBER pfn;
  ULONG qemu_hide_filter;
  ULONG qemu_hide_flags_value;
  int i;
  ULONG state;
  ULONG octet;
  PCHAR tmp_string;
  ULONG tmp_ulong;

  if (!suspend) {
    xi->handle = XnOpenDevice(xi->pdo, XenNet_DeviceCallback, xi);
  }
  if (!xi->handle) {
    FUNCTION_MSG("Cannot open Xen device\n");
    return STATUS_UNSUCCESSFUL;
  }
  XnGetValue(xi->handle, XN_VALUE_TYPE_QEMU_HIDE_FLAGS, &qemu_hide_flags_value);
  XnGetValue(xi->handle, XN_VALUE_TYPE_QEMU_FILTER, &qemu_hide_filter);
  if (!(qemu_hide_flags_value & QEMU_UNPLUG_ALL_NICS) || qemu_hide_filter) {
    FUNCTION_MSG("inactive\n");
    xi->device_state = DEVICE_STATE_INACTIVE;
    XnCloseDevice(xi->handle);
    return STATUS_SUCCESS;
  }

  for (i = 0; i <= 5 && xi->backend_state != XenbusStateInitialising && xi->backend_state != XenbusStateInitWait && xi->backend_state != XenbusStateInitialised; i++) {
    FUNCTION_MSG("Waiting for XenbusStateInitXxx\n");
    KeWaitForSingleObject(&xi->backend_event, Executive, KernelMode, FALSE, NULL);
  }
  if (xi->backend_state != XenbusStateInitialising && xi->backend_state != XenbusStateInitWait && xi->backend_state != XenbusStateInitialised) {
    FUNCTION_MSG("Backend state timeout\n");
    return STATUS_UNSUCCESSFUL;
  }
  if (!NT_SUCCESS(status = XnBindEvent(xi->handle, &xi->event_channel, XenNet_HandleEvent_DIRQL, xi))) {
    FUNCTION_MSG("Cannot allocate event channel\n");
    return STATUS_UNSUCCESSFUL;
  }
  FUNCTION_MSG("event_channel = %d\n", xi->event_channel);
  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "event-channel", xi->event_channel);
  xi->tx_sring = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, XENNET_POOL_TAG);
  if (!xi->tx_sring) {
    FUNCTION_MSG("Cannot allocate tx_sring\n");
    return STATUS_UNSUCCESSFUL;
  }
  SHARED_RING_INIT(xi->tx_sring);
  FRONT_RING_INIT(&xi->tx_ring, xi->tx_sring, PAGE_SIZE);
  pfn = MmGetPhysicalAddress(xi->tx_sring).QuadPart >> PAGE_SHIFT;
  FUNCTION_MSG("tx sring pfn = %d\n", (ULONG)pfn);
  xi->tx_sring_gref = XnGrantAccess(xi->handle, (ULONG)pfn, FALSE, INVALID_GRANT_REF, XENNET_POOL_TAG);
  FUNCTION_MSG("tx sring_gref = %d\n", xi->tx_sring_gref);
  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "tx-ring-ref", xi->tx_sring_gref);  
  xi->rx_sring = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, XENNET_POOL_TAG);
  if (!xi->rx_sring) {
    FUNCTION_MSG("Cannot allocate rx_sring\n");
    return STATUS_UNSUCCESSFUL;
  }
  SHARED_RING_INIT(xi->rx_sring);
  FRONT_RING_INIT(&xi->rx_ring, xi->rx_sring, PAGE_SIZE);
  pfn = MmGetPhysicalAddress(xi->rx_sring).QuadPart >> PAGE_SHIFT;
  FUNCTION_MSG("rx sring pfn = %d\n", (ULONG)pfn);
  xi->rx_sring_gref = XnGrantAccess(xi->handle, (ULONG)pfn, FALSE, INVALID_GRANT_REF, XENNET_POOL_TAG);
  FUNCTION_MSG("rx sring_gref = %d\n", xi->rx_sring_gref);
  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "rx-ring-ref", xi->rx_sring_gref);  

  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "request-rx-copy", 1);
  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "request-rx-notify", 1);
  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "feature-no-csum-offload", !xi->frontend_csum_supported);
  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "feature-sg", (int)xi->frontend_sg_supported);
  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "feature-gso-tcpv4", !!xi->frontend_gso_value);

  status = XnReadInt32(xi->handle, XN_BASE_BACKEND, "feature-sg", &tmp_ulong);
  if (tmp_ulong) {
    xi->backend_sg_supported = TRUE;
  }
  status = XnReadInt32(xi->handle, XN_BASE_BACKEND, "feature-gso-tcpv4", &tmp_ulong);
  if (tmp_ulong) {
    xi->backend_gso_value = xi->frontend_gso_value;
  }

  status = XnReadString(xi->handle, XN_BASE_BACKEND, "mac", &tmp_string);
  state = 0;
  octet = 0;
  for (i = 0; state != 3 && i < strlen(tmp_string); i++) {
    if (octet == 6) {
      state = 3;
      break;
    }
    switch(state) {
    case 0:
    case 1:
      if (tmp_string[i] >= '0' && tmp_string[i] <= '9') {
        xi->perm_mac_addr[octet] |= (tmp_string[i] - '0') << ((1 - state) * 4);
        state++;
      } else if (tmp_string[i] >= 'A' && tmp_string[i] <= 'F') {
        xi->perm_mac_addr[octet] |= (tmp_string[i] - 'A' + 10) << ((1 - state) * 4);
        state++;
      } else if (tmp_string[i] >= 'a' && tmp_string[i] <= 'f') {
        xi->perm_mac_addr[octet] |= (tmp_string[i] - 'a' + 10) << ((1 - state) * 4);
        state++;
      } else {
        state = 3;
      }
      break;
    case 2:
      if (tmp_string[i] == ':') {
        octet++;
        state = 0;
      } else {
        state = 3;
      }
      break;
    }
  }
  if (octet != 5 || state != 2) {
    FUNCTION_MSG("Failed to parse backend MAC address %s\n", tmp_string);
    XnFreeMem(xi->handle, tmp_string);
    return STATUS_UNSUCCESSFUL;
  } else if ((xi->curr_mac_addr[0] & 0x03) != 0x02) {
    /* only copy if curr_mac_addr is not a LUA */
    memcpy(xi->curr_mac_addr, xi->perm_mac_addr, ETH_ALEN);
  }
  XnFreeMem(xi->handle, tmp_string);
  FUNCTION_MSG("MAC address is %02X:%02X:%02X:%02X:%02X:%02X\n",
    xi->curr_mac_addr[0], xi->curr_mac_addr[1], xi->curr_mac_addr[2], 
    xi->curr_mac_addr[3], xi->curr_mac_addr[4], xi->curr_mac_addr[5]);

  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "state", XenbusStateConnected);

  for (i = 0; i <= 5 && xi->backend_state != XenbusStateConnected; i++) {
    FUNCTION_MSG("Waiting for XenbusStateConnected\n");
    KeWaitForSingleObject(&xi->backend_event, Executive, KernelMode, FALSE, NULL);
  }
  if (xi->backend_state != XenbusStateConnected) {
    FUNCTION_MSG("Backend state timeout\n");
    return STATUS_UNSUCCESSFUL;
  }
  XenNet_TxInit(xi);
  XenNet_RxInit(xi);

  /* we don't set device_state = DEVICE_STATE_ACTIVE here - has to be done during init once ndis is ready */
  
  return STATUS_SUCCESS;
}

static NTSTATUS
XenNet_Disconnect(PVOID context, BOOLEAN suspend) {
  struct xennet_info *xi = (struct xennet_info *)context;
  PFN_NUMBER pfn;
  NTSTATUS status;

  if (xi->device_state != DEVICE_STATE_ACTIVE) {
    FUNCTION_MSG("state not DEVICE_STATE_ACTIVE, is %d instead\n", xi->device_state);
    FUNCTION_EXIT();
    return STATUS_SUCCESS;
  }
  xi->device_state = DEVICE_STATE_DISCONNECTING;
  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "state", XenbusStateClosing);
  while (xi->backend_state != XenbusStateClosing && xi->backend_state != XenbusStateClosed) {
    FUNCTION_MSG("Waiting for XenbusStateClosing/Closed\n");
    KeWaitForSingleObject(&xi->backend_event, Executive, KernelMode, FALSE, NULL);
  }
  status = XnWriteInt32(xi->handle, XN_BASE_FRONTEND, "state", XenbusStateClosed);
  while (xi->backend_state != XenbusStateClosed) {
    FUNCTION_MSG("Waiting for XenbusStateClosed\n");
    KeWaitForSingleObject(&xi->backend_event, Executive, KernelMode, FALSE, NULL);
  }
  XnUnbindEvent(xi->handle, xi->event_channel);
  KeFlushQueuedDpcs();
  XenNet_TxShutdown(xi);
  XenNet_RxShutdown(xi);
  pfn = MmGetPhysicalAddress(xi->rx_sring).QuadPart >> PAGE_SHIFT;
  XnEndAccess(xi->handle, xi->rx_sring_gref, FALSE, XENNET_POOL_TAG);
  ExFreePoolWithTag(xi->rx_sring, XENNET_POOL_TAG);
  pfn = MmGetPhysicalAddress(xi->tx_sring).QuadPart >> PAGE_SHIFT;
  XnEndAccess(xi->handle, xi->tx_sring_gref, FALSE, XENNET_POOL_TAG);
  ExFreePoolWithTag(xi->tx_sring, XENNET_POOL_TAG);
  if (!suspend) {
    XnCloseDevice(xi->handle);
  }
  xi->device_state = DEVICE_STATE_DISCONNECTED;
  return STATUS_SUCCESS;
};

static VOID
XenNet_DeviceCallback(PVOID context, ULONG callback_type, PVOID value) {
  struct xennet_info *xi = (struct xennet_info *)context;
  ULONG state;
  
  FUNCTION_ENTER();
  switch (callback_type) {
  case XN_DEVICE_CALLBACK_BACKEND_STATE:
    state = (ULONG)(ULONG_PTR)value;
    if (state == xi->backend_state) {
      FUNCTION_MSG("same state %d\n", state);
      FUNCTION_EXIT();
    }
    FUNCTION_MSG("XenBusState = %d -> %d\n", xi->backend_state, state);
    xi->backend_state = state;
    KeSetEvent(&xi->backend_event, 0, FALSE);
    break;
  case XN_DEVICE_CALLBACK_SUSPEND:
    FUNCTION_MSG("XN_DEVICE_CALLBACK_SUSPEND");
    XenNet_Disconnect(xi, TRUE);
    break;
  case XN_DEVICE_CALLBACK_RESUME:
    FUNCTION_MSG("XN_DEVICE_CALLBACK_RESUME");
    XenNet_Connect(xi, TRUE);
    xi->device_state = DEVICE_STATE_ACTIVE;
    KeInsertQueueDpc(&xi->rxtx_dpc, NULL, NULL);
    break;
  }
  FUNCTION_EXIT();
}

// Called at PASSIVE_LEVEL
static NDIS_STATUS
XenNet_Initialize(NDIS_HANDLE adapter_handle, NDIS_HANDLE driver_context, PNDIS_MINIPORT_INIT_PARAMETERS init_parameters)
{
  NDIS_STATUS status;
  struct xennet_info *xi = NULL;
  NDIS_HANDLE config_handle;
  NDIS_STRING config_param_name;
  NDIS_CONFIGURATION_OBJECT config_object;
  PNDIS_CONFIGURATION_PARAMETER config_param;
  //PNDIS_MINIPORT_ADAPTER_ATTRIBUTES adapter_attributes;
  ULONG i;
  ULONG length;
  PVOID network_address;
  UINT network_address_length;
  NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES registration_attributes;
  NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES general_attributes;
  NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES offload_attributes;
  NDIS_OFFLOAD df_offload, hw_offload;
  NDIS_TCP_CONNECTION_OFFLOAD df_conn_offload, hw_conn_offload;
  static NDIS_OID *supported_oids;

  UNREFERENCED_PARAMETER(driver_context);
  UNREFERENCED_PARAMETER(init_parameters);

  FUNCTION_ENTER();

  /* Alloc memory for adapter private info */
  status = NdisAllocateMemoryWithTag((PVOID)&xi, sizeof(*xi), XENNET_POOL_TAG);
  if (!NT_SUCCESS(status))  {
    FUNCTION_MSG("NdisAllocateMemoryWithTag failed with 0x%x\n", status);
    status = NDIS_STATUS_RESOURCES;
    goto err;
  }
  RtlZeroMemory(xi, sizeof(*xi));
  xi->adapter_handle = adapter_handle;
  xi->device_state = DEVICE_STATE_INITIALISING;
  NdisMGetDeviceProperty(xi->adapter_handle, &xi->pdo, &xi->fdo,
    &xi->lower_do, NULL, NULL);
  KeInitializeEvent(&xi->backend_event, SynchronizationEvent, FALSE);

  xi->rx_target     = RX_DFL_MIN_TARGET;
  xi->rx_min_target = RX_DFL_MIN_TARGET;
  xi->rx_max_target = RX_MAX_TARGET;
  
  xi->multicast_list_size = 0;
  xi->current_lookahead = MIN_LOOKAHEAD_LENGTH;

  xi->stats.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
  xi->stats.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
  xi->stats.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
  xi->stats.SupportedStatistics = NDIS_STATISTICS_XMIT_OK_SUPPORTED
    | NDIS_STATISTICS_RCV_OK_SUPPORTED
    | NDIS_STATISTICS_XMIT_ERROR_SUPPORTED
    | NDIS_STATISTICS_RCV_ERROR_SUPPORTED
    | NDIS_STATISTICS_RCV_NO_BUFFER_SUPPORTED
    | NDIS_STATISTICS_DIRECTED_BYTES_XMIT_SUPPORTED
    | NDIS_STATISTICS_DIRECTED_FRAMES_XMIT_SUPPORTED
    | NDIS_STATISTICS_MULTICAST_BYTES_XMIT_SUPPORTED
    | NDIS_STATISTICS_MULTICAST_FRAMES_XMIT_SUPPORTED
    | NDIS_STATISTICS_BROADCAST_BYTES_XMIT_SUPPORTED
    | NDIS_STATISTICS_BROADCAST_FRAMES_XMIT_SUPPORTED
    | NDIS_STATISTICS_DIRECTED_BYTES_RCV_SUPPORTED
    | NDIS_STATISTICS_DIRECTED_FRAMES_RCV_SUPPORTED
    | NDIS_STATISTICS_MULTICAST_BYTES_RCV_SUPPORTED
    | NDIS_STATISTICS_MULTICAST_FRAMES_RCV_SUPPORTED
    | NDIS_STATISTICS_BROADCAST_BYTES_RCV_SUPPORTED
    | NDIS_STATISTICS_BROADCAST_FRAMES_RCV_SUPPORTED
    | NDIS_STATISTICS_RCV_CRC_ERROR_SUPPORTED
    | NDIS_STATISTICS_TRANSMIT_QUEUE_LENGTH_SUPPORTED
    | NDIS_STATISTICS_BYTES_RCV_SUPPORTED
    | NDIS_STATISTICS_BYTES_XMIT_SUPPORTED
    | NDIS_STATISTICS_RCV_DISCARDS_SUPPORTED
    | NDIS_STATISTICS_GEN_STATISTICS_SUPPORTED
    | NDIS_STATISTICS_XMIT_DISCARDS_SUPPORTED;

  KeInitializeDpc(&xi->rxtx_dpc, XenNet_RxTxDpc, xi);

  xi->packet_filter = 0;

  status = IoGetDeviceProperty(xi->pdo, DevicePropertyDeviceDescription,
    NAME_SIZE, xi->dev_desc, &length);
  if (!NT_SUCCESS(status)) {
    KdPrint(("IoGetDeviceProperty failed with 0x%x\n", status));
    status = NDIS_STATUS_FAILURE;
    goto err;
  }

  //xi->power_state = NdisDeviceStateD0;
  //xi->power_workitem = IoAllocateWorkItem(xi->fdo);

  config_object.Header.Size = sizeof(NDIS_CONFIGURATION_OBJECT);
  config_object.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
  config_object.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
  config_object.NdisHandle = xi->adapter_handle;
  config_object.Flags = 0;

  status = NdisOpenConfigurationEx(&config_object, &config_handle);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("Could not open config in registry (%08x)\n", status);
    status = NDIS_STATUS_RESOURCES;
    goto err;
  }

  NdisInitUnicodeString(&config_param_name, L"ScatterGather");
  NdisReadConfiguration(&status, &config_param, config_handle, &config_param_name, NdisParameterInteger);
  if (!NT_SUCCESS(status))
  {
    FUNCTION_MSG("Could not read ScatterGather value (%08x)\n", status);
    xi->frontend_sg_supported = TRUE;
  } else {
    FUNCTION_MSG("ScatterGather = %d\n", config_param->ParameterData.IntegerData);
    xi->frontend_sg_supported = !!config_param->ParameterData.IntegerData;
  }
  if (xi->frontend_sg_supported && ndis_os_minor_version < 1) {
    FUNCTION_MSG("No support for SG with NDIS 6.0, disabled\n");
    xi->frontend_sg_supported = FALSE;
  }
  
  NdisInitUnicodeString(&config_param_name, L"LargeSendOffload");
  NdisReadConfiguration(&status, &config_param, config_handle, &config_param_name, NdisParameterInteger);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("Could not read LargeSendOffload value (%08x)\n", status);
    xi->frontend_gso_value = 0;
  } else {
    FUNCTION_MSG("LargeSendOffload = %d\n", config_param->ParameterData.IntegerData);
    xi->frontend_gso_value = config_param->ParameterData.IntegerData;
    if (xi->frontend_gso_value > 61440) {
      xi->frontend_gso_value = 61440;
      FUNCTION_MSG("  (clipped to %d)\n", xi->frontend_gso_value);
    }
    if (!xi->frontend_sg_supported && xi->frontend_gso_value > PAGE_SIZE - MAX_PKT_HEADER_LENGTH) {
      /* without SG, GSO can be a maximum of PAGE_SIZE - MAX_PKT_HEADER_LENGTH */
      xi->frontend_gso_value = min(xi->frontend_gso_value, PAGE_SIZE - MAX_PKT_HEADER_LENGTH);
      FUNCTION_MSG("  (clipped to %d with sg disabled)\n", xi->frontend_gso_value);
    }
  }
  if (xi->frontend_sg_supported && ndis_os_minor_version < 1) {
    FUNCTION_MSG("No support for GSO with NDIS 6.0, disabled\n");
    xi->frontend_gso_value = 0;
  }

  NdisInitUnicodeString(&config_param_name, L"LargeSendOffloadRxSplitMTU");
  NdisReadConfiguration(&status, &config_param, config_handle, &config_param_name, NdisParameterInteger);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("Could not read LargeSendOffload value (%08x)\n", status);
    xi->frontend_gso_rx_split_type = RX_LSO_SPLIT_HALF;
  } else {
    FUNCTION_MSG("LargeSendOffloadRxSplitMTU = %d\n", config_param->ParameterData.IntegerData);
    switch (config_param->ParameterData.IntegerData) {
    case RX_LSO_SPLIT_MSS:
    case RX_LSO_SPLIT_HALF:
    case RX_LSO_SPLIT_NONE:
      xi->frontend_gso_rx_split_type = config_param->ParameterData.IntegerData;
      break;
    default:
      xi->frontend_gso_rx_split_type = RX_LSO_SPLIT_HALF;
      break;
    }
  }

  NdisInitUnicodeString(&config_param_name, L"ChecksumOffload");
  NdisReadConfiguration(&status, &config_param, config_handle, &config_param_name, NdisParameterInteger);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("Could not read ChecksumOffload value (%08x)\n", status);
    xi->frontend_csum_supported = TRUE;
  } else {
    FUNCTION_MSG("ChecksumOffload = %d\n", config_param->ParameterData.IntegerData);
    xi->frontend_csum_supported = !!config_param->ParameterData.IntegerData;
  }

  NdisInitUnicodeString(&config_param_name, L"MTU");
  NdisReadConfiguration(&status, &config_param, config_handle, &config_param_name, NdisParameterInteger);  
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("Could not read MTU value (%08x)\n", status);
    xi->frontend_mtu_value = 1500;
  } else {
    FUNCTION_MSG("MTU = %d\n", config_param->ParameterData.IntegerData);
    xi->frontend_mtu_value = config_param->ParameterData.IntegerData;
  }
  
  NdisReadNetworkAddress(&status, &network_address, &network_address_length, config_handle);
  if (!NT_SUCCESS(status) || network_address_length != ETH_ALEN || ((((PUCHAR)network_address)[0] & 0x03) != 0x02)) {
    FUNCTION_MSG("Could not read registry NetworkAddress value (%08x) or value is invalid\n", status);
    memset(xi->curr_mac_addr, 0, ETH_ALEN);
  } else {
    memcpy(xi->curr_mac_addr, network_address, ETH_ALEN);
    FUNCTION_MSG("Set MAC address from registry to %02X:%02X:%02X:%02X:%02X:%02X\n",
      xi->curr_mac_addr[0], xi->curr_mac_addr[1], xi->curr_mac_addr[2], 
      xi->curr_mac_addr[3], xi->curr_mac_addr[4], xi->curr_mac_addr[5]);
  }

  NdisCloseConfiguration(config_handle);

  XenNet_Connect(xi, FALSE);

  if (!xi->backend_sg_supported)
    xi->backend_gso_value = min(xi->backend_gso_value, PAGE_SIZE - MAX_PKT_HEADER_LENGTH);

  xi->current_sg_supported = xi->frontend_sg_supported && xi->backend_sg_supported;
  xi->current_csum_supported = xi->frontend_csum_supported && xi->backend_csum_supported;
  xi->current_gso_value = min(xi->backend_gso_value, xi->backend_gso_value);
  xi->current_mtu_value = xi->frontend_mtu_value;
  xi->current_gso_rx_split_type = xi->frontend_gso_rx_split_type;
  
  xi->config_max_pkt_size = max(xi->current_mtu_value + XN_HDR_SIZE, xi->current_gso_value + XN_HDR_SIZE);
    
  registration_attributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
  registration_attributes.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
  registration_attributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
  registration_attributes.MiniportAdapterContext = xi;
  registration_attributes.AttributeFlags = 0;
  registration_attributes.AttributeFlags |= NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE;
  registration_attributes.AttributeFlags |= NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK;
  registration_attributes.CheckForHangTimeInSeconds = 0; /* use default */
  registration_attributes.InterfaceType = NdisInterfacePNPBus;
  status = NdisMSetMiniportAttributes(xi->adapter_handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&registration_attributes);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("NdisMSetMiniportAttributes(registration) failed (%08x)\n", status);
    goto err;
  }
  
  general_attributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
  general_attributes.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1; /* revision 2 is NDIS 6.2 */
  general_attributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
  general_attributes.Flags = 0;
  general_attributes.MediaType = NdisMedium802_3;
  general_attributes.PhysicalMediumType = NdisPhysicalMediumOther;
  general_attributes.MtuSize = xi->current_mtu_value;
  general_attributes.MaxXmitLinkSpeed = MAX_LINK_SPEED;
  general_attributes.XmitLinkSpeed = MAX_LINK_SPEED;
  general_attributes.MaxRcvLinkSpeed = MAX_LINK_SPEED;
  general_attributes.RcvLinkSpeed = MAX_LINK_SPEED;
  general_attributes.MediaConnectState = MediaConnectStateConnected;
  general_attributes.MediaDuplexState = MediaDuplexStateFull;
  general_attributes.LookaheadSize = xi->current_lookahead;
  general_attributes.PowerManagementCapabilities = NULL;
  general_attributes.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | 
        NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
        NDIS_MAC_OPTION_NO_LOOPBACK;
  general_attributes.SupportedPacketFilters = SUPPORTED_PACKET_FILTERS;
  general_attributes.MaxMulticastListSize = MULTICAST_LIST_MAX_SIZE;
  general_attributes.MacAddressLength = 6;
  NdisMoveMemory(general_attributes.PermanentMacAddress, xi->perm_mac_addr, general_attributes.MacAddressLength);
  NdisMoveMemory(general_attributes.CurrentMacAddress, xi->curr_mac_addr, general_attributes.MacAddressLength);
  general_attributes.RecvScaleCapabilities = NULL; /* we do want to support this soon */
  general_attributes.AccessType = NET_IF_ACCESS_BROADCAST;
  general_attributes.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
  general_attributes.ConnectionType = NET_IF_CONNECTION_DEDICATED;
  general_attributes.IfType = IF_TYPE_ETHERNET_CSMACD;
  general_attributes.IfConnectorPresent = TRUE;
  general_attributes.SupportedStatistics = xi->stats.SupportedStatistics;
  general_attributes.SupportedPauseFunctions = NdisPauseFunctionsUnsupported;
  general_attributes.DataBackFillSize = 0; // see NdisRetreatNetBufferDataStart
  general_attributes.ContextBackFillSize = 0; // ?? NFI ??
  
  for (i = 0; xennet_oids[i].oid; i++);
  
  status = NdisAllocateMemoryWithTag((PVOID)&supported_oids, sizeof(NDIS_OID) * i, XENNET_POOL_TAG);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("NdisAllocateMemoryWithTag failed with 0x%x\n", status);
    status = NDIS_STATUS_RESOURCES;
    goto err;
  }

  for (i = 0; xennet_oids[i].oid; i++) {
    supported_oids[i] = xennet_oids[i].oid;
    FUNCTION_MSG("Supporting %08x (%s) %s %d bytes\n", xennet_oids[i].oid, xennet_oids[i].oid_name, (xennet_oids[i].query_routine?(xennet_oids[i].set_routine?"get/set":"get only"):(xennet_oids[i].set_routine?"set only":"none")), xennet_oids[i].min_length);
  }
  general_attributes.SupportedOidList = supported_oids;
  general_attributes.SupportedOidListLength = sizeof(NDIS_OID) * i;
  general_attributes.AutoNegotiationFlags = NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED
    | NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED
    | NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED;
  //general_attributes.PowerManagementCapabilitiesEx = NULL; // >= 6.20
  status = NdisMSetMiniportAttributes(xi->adapter_handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&general_attributes);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("NdisMSetMiniportAttributes(general) failed (%08x)\n", status);
    goto err;
  }
  NdisFreeMemory(supported_oids, 0, 0);
    
  /* this is the initial offload state */
  RtlZeroMemory(&df_offload, sizeof(df_offload));
  df_offload.Header.Type = NDIS_OBJECT_TYPE_OFFLOAD;
  df_offload.Header.Revision = NDIS_OFFLOAD_REVISION_1; // revision 2 does exist
  df_offload.Header.Size = NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_1;
  /* this is the supported offload state */
  RtlZeroMemory(&hw_offload, sizeof(hw_offload));
  hw_offload.Header.Type = NDIS_OBJECT_TYPE_OFFLOAD;
  hw_offload.Header.Revision = NDIS_OFFLOAD_REVISION_1; // revision 2 does exist
  hw_offload.Header.Size = NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_1;
  if (xi->current_csum_supported)
  {
    df_offload.Checksum.IPv4Transmit.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    df_offload.Checksum.IPv4Transmit.IpOptionsSupported = NDIS_OFFLOAD_SET_ON;
    df_offload.Checksum.IPv4Transmit.TcpOptionsSupported = NDIS_OFFLOAD_SET_ON;
    df_offload.Checksum.IPv4Transmit.TcpChecksum = NDIS_OFFLOAD_SET_ON;
    df_offload.Checksum.IPv4Transmit.UdpChecksum = NDIS_OFFLOAD_SET_ON;
    df_offload.Checksum.IPv4Transmit.IpChecksum = NDIS_OFFLOAD_SET_ON;
    df_offload.Checksum.IPv4Receive.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    df_offload.Checksum.IPv4Receive.IpOptionsSupported = NDIS_OFFLOAD_SET_ON;
    df_offload.Checksum.IPv4Receive.TcpOptionsSupported = NDIS_OFFLOAD_SET_ON;
    df_offload.Checksum.IPv4Receive.TcpChecksum = NDIS_OFFLOAD_SET_ON;
    df_offload.Checksum.IPv4Receive.UdpChecksum = NDIS_OFFLOAD_SET_ON;
    df_offload.Checksum.IPv4Receive.IpChecksum = NDIS_OFFLOAD_SET_ON;
    /* offload.Checksum.IPv6Transmit is not supported */
    /* offload.Checksum.IPv6Receive is not supported */
    hw_offload.Checksum.IPv4Transmit.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hw_offload.Checksum.IPv4Transmit.IpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hw_offload.Checksum.IPv4Transmit.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hw_offload.Checksum.IPv4Transmit.TcpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hw_offload.Checksum.IPv4Transmit.UdpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hw_offload.Checksum.IPv4Transmit.IpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hw_offload.Checksum.IPv4Receive.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hw_offload.Checksum.IPv4Receive.IpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hw_offload.Checksum.IPv4Receive.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hw_offload.Checksum.IPv4Receive.TcpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hw_offload.Checksum.IPv4Receive.UdpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hw_offload.Checksum.IPv4Receive.IpChecksum = NDIS_OFFLOAD_SUPPORTED;
    /* hw_offload.Checksum.IPv6Transmit is not supported */
    /* hw_offload.Checksum.IPv6Receive is not supported */
  }
  if (xi->current_gso_value)
  {
    hw_offload.LsoV1.IPv4.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hw_offload.LsoV1.IPv4.MaxOffLoadSize = xi->current_gso_value;
    hw_offload.LsoV1.IPv4.MinSegmentCount = MIN_LARGE_SEND_SEGMENTS;
    hw_offload.LsoV1.IPv4.TcpOptions = NDIS_OFFLOAD_NOT_SUPPORTED; /* linux can't handle this */
    hw_offload.LsoV1.IPv4.IpOptions = NDIS_OFFLOAD_NOT_SUPPORTED; /* linux can't handle this */
    hw_offload.LsoV2.IPv4.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hw_offload.LsoV2.IPv4.MaxOffLoadSize = xi->current_gso_value;
    hw_offload.LsoV2.IPv4.MinSegmentCount = MIN_LARGE_SEND_SEGMENTS;
    /* hw_offload.LsoV2.IPv6 is not supported */
    df_offload.LsoV1.IPv4.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    df_offload.LsoV1.IPv4.MaxOffLoadSize = xi->current_gso_value;
    df_offload.LsoV1.IPv4.MinSegmentCount = MIN_LARGE_SEND_SEGMENTS;
    df_offload.LsoV1.IPv4.TcpOptions = NDIS_OFFLOAD_NOT_SUPPORTED; /* linux can't handle this */
    df_offload.LsoV1.IPv4.IpOptions = NDIS_OFFLOAD_NOT_SUPPORTED; /* linux can't handle this */
    df_offload.LsoV2.IPv4.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    df_offload.LsoV2.IPv4.MaxOffLoadSize = xi->current_gso_value;
    df_offload.LsoV2.IPv4.MinSegmentCount = MIN_LARGE_SEND_SEGMENTS;
    /* df_offload.LsoV2.IPv6 is not supported */
  }
  /* hw_offload.IPsecV1 is not supported */
  /* hw_offload.IPsecV2 is not supported */
  /* df_offload.IPsecV1 is not supported */
  /* df_offload.IPsecV2 is not supported */
  hw_offload.Flags = 0;
  df_offload.Flags = 0;
  
  RtlZeroMemory(&df_conn_offload, sizeof(df_conn_offload));
  df_conn_offload.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
  df_conn_offload.Header.Revision = NDIS_TCP_CONNECTION_OFFLOAD_REVISION_1;
  df_conn_offload.Header.Size = NDIS_SIZEOF_TCP_CONNECTION_OFFLOAD_REVISION_1;

  RtlZeroMemory(&hw_conn_offload, sizeof(hw_conn_offload));
  hw_conn_offload.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
  hw_conn_offload.Header.Revision = NDIS_TCP_CONNECTION_OFFLOAD_REVISION_1;
  hw_conn_offload.Header.Size = NDIS_SIZEOF_TCP_CONNECTION_OFFLOAD_REVISION_1;
  
  offload_attributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES;
  offload_attributes.Header.Revision = NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1;
  offload_attributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1;
  offload_attributes.DefaultOffloadConfiguration = &df_offload;
  offload_attributes.HardwareOffloadCapabilities = &hw_offload;
  offload_attributes.DefaultTcpConnectionOffloadConfiguration = &df_conn_offload;
  offload_attributes.TcpConnectionOffloadHardwareCapabilities  = &hw_conn_offload;
  status = NdisMSetMiniportAttributes(xi->adapter_handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&offload_attributes);
  if (!NT_SUCCESS(status)) {
    FUNCTION_MSG("NdisMSetMiniportAttributes(offload) failed (%08x)\n", status);
    goto err;
  }
  
  #if 0
  if (ndis_os_minor_version >= 1) {
    NDIS_MINIPORT_ADAPTER_HARDWARE_ASSIST_ATTRIBUTES hw_assist_attributes;
    NDIS_HD_SPLIT_ATTRIBUTES hd_split_attributes;
    
    RtlZeroMemory(&hd_split_attributes, sizeof(hd_split_attributes));
    hd_split_attributes.Header.Type = NDIS_OBJECT_TYPE_HD_SPLIT_ATTRIBUTES;
    hd_split_attributes.Header.Revision = NDIS_HD_SPLIT_ATTRIBUTES_REVISION_1;
    hd_split_attributes.Header.Size = NDIS_SIZEOF_HD_SPLIT_ATTRIBUTES_REVISION_1;
    hd_split_attributes.HardwareCapabilities = NDIS_HD_SPLIT_CAPS_SUPPORTS_HEADER_DATA_SPLIT | NDIS_HD_SPLIT_CAPS_SUPPORTS_IPV4_OPTIONS | NDIS_HD_SPLIT_CAPS_SUPPORTS_TCP_OPTIONS;
    hd_split_attributes.CurrentCapabilities = hd_split_attributes.HardwareCapabilities;
    /* the other members are set on output */
    
    RtlZeroMemory(&hw_assist_attributes, sizeof(hw_assist_attributes));
    hw_assist_attributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_HARDWARE_ASSIST_ATTRIBUTES;
    hw_assist_attributes.Header.Revision = NDIS_MINIPORT_ADAPTER_HARDWARE_ASSIST_ATTRIBUTES_REVISION_1;
    hw_assist_attributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_HARDWARE_ASSIST_ATTRIBUTES_REVISION_1;
    hw_assist_attributes.HDSplitAttributes = &hd_split_attributes;
    status = NdisMSetMiniportAttributes(xi->adapter_handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&hw_assist_attributes);
    if (!NT_SUCCESS(status))
    {
      FUNCTION_MSG("NdisMSetMiniportAttributes(hw_assist) failed (%08x)\n", status);
      goto err;
    }
    FUNCTION_MSG("HW Split enabled\n");
    FUNCTION_MSG(" HDSplitFlags = %08x\n", hd_split_attributes.HDSplitFlags);
    FUNCTION_MSG(" BackfillSize = %d\n", hd_split_attributes.BackfillSize);
    FUNCTION_MSG(" MaxHeaderSize = %d\n", hd_split_attributes.MaxHeaderSize);
    //what about backfill here?
  }
  #endif
  xi->device_state = DEVICE_STATE_ACTIVE;
  FUNCTION_EXIT();
  return NDIS_STATUS_SUCCESS;
  
err:
  if (xi) {
    NdisFreeMemory(xi, 0, 0);
  }
  FUNCTION_EXIT_STATUS(status);

  return status;
}

static VOID
XenNet_DevicePnPEventNotify(NDIS_HANDLE adapter_context, PNET_DEVICE_PNP_EVENT pnp_event)
{
  UNREFERENCED_PARAMETER(adapter_context);

  FUNCTION_ENTER();
  switch (pnp_event->DevicePnPEvent)
  {
  case NdisDevicePnPEventSurpriseRemoved:
    FUNCTION_MSG("NdisDevicePnPEventSurpriseRemoved\n");
    break;
  case NdisDevicePnPEventPowerProfileChanged :
    FUNCTION_MSG("NdisDevicePnPEventPowerProfileChanged\n");
    break;
  default:
    FUNCTION_MSG("NdisDevicePnPEvent%d\n", pnp_event->DevicePnPEvent);
    break;
  }
  FUNCTION_EXIT();
}

/* called at <= HIGH_IRQL, or PASSIVE_LEVEL, depending on shutdown_action */
VOID
XenNet_Shutdown(NDIS_HANDLE adapter_context, NDIS_SHUTDOWN_ACTION shutdown_action)
{
  UNREFERENCED_PARAMETER(adapter_context);
  UNREFERENCED_PARAMETER(shutdown_action);

  FUNCTION_ENTER();
  FUNCTION_EXIT();
}

static BOOLEAN
XenNet_CheckForHang(NDIS_HANDLE adapter_context)
{
  UNREFERENCED_PARAMETER(adapter_context);

  //FUNCTION_ENTER();
  //FUNCTION_EXIT();
  return FALSE;
}

/* Opposite of XenNet_Init */
static VOID
XenNet_Halt(NDIS_HANDLE adapter_context, NDIS_HALT_ACTION halt_action)
{
  struct xennet_info *xi = adapter_context;
  UNREFERENCED_PARAMETER(halt_action);

  FUNCTION_ENTER();
  XenNet_Disconnect(xi, FALSE);
  NdisFreeMemory(xi, 0, 0);

  FUNCTION_EXIT();
}

static NDIS_STATUS 
XenNet_Reset(NDIS_HANDLE adapter_context, PBOOLEAN addressing_reset)
{
  UNREFERENCED_PARAMETER(adapter_context);

  FUNCTION_ENTER();
  *addressing_reset = FALSE;
  FUNCTION_EXIT();
  return NDIS_STATUS_SUCCESS;
}

/* called at PASSIVE_LEVEL */
static NDIS_STATUS
XenNet_Pause(NDIS_HANDLE adapter_context, PNDIS_MINIPORT_PAUSE_PARAMETERS pause_parameters)
{
  UNREFERENCED_PARAMETER(adapter_context);
  UNREFERENCED_PARAMETER(pause_parameters);
  FUNCTION_ENTER();
  FUNCTION_EXIT();
  return STATUS_SUCCESS;
}

/* called at PASSIVE_LEVEL */
static NDIS_STATUS
XenNet_Restart(NDIS_HANDLE adapter_context, PNDIS_MINIPORT_RESTART_PARAMETERS restart_parameters)
{
  UNREFERENCED_PARAMETER(adapter_context);
  UNREFERENCED_PARAMETER(restart_parameters);
  FUNCTION_ENTER();
  FUNCTION_EXIT();
  return STATUS_SUCCESS;
}

static VOID
XenNet_Unload(PDRIVER_OBJECT driver_object)
{
  UNREFERENCED_PARAMETER(driver_object);
  FUNCTION_ENTER();
  NdisMDeregisterMiniportDriver(driver_handle);
  FUNCTION_EXIT();
}

static NDIS_STATUS
XenNet_SetOptions(NDIS_HANDLE driver_handle, NDIS_HANDLE driver_context)
{
  UNREFERENCED_PARAMETER(driver_handle);
  UNREFERENCED_PARAMETER(driver_context);
  FUNCTION_ENTER();
  FUNCTION_EXIT();
  return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path)
{
  NTSTATUS status;  
  NDIS_MINIPORT_DRIVER_CHARACTERISTICS mini_chars;
  ULONG ndis_version;
  
  FUNCTION_ENTER();

  ndis_version = NdisGetVersion();
  
  ndis_os_major_version = ndis_version >> 16;
  ndis_os_minor_version = ndis_version & 0xFFFF;

  NdisZeroMemory(&mini_chars, sizeof(mini_chars));

  mini_chars.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
  
  if (ndis_os_minor_version < 1) {
    mini_chars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_1;
    mini_chars.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_1;

    mini_chars.MajorNdisVersion = 6;
    mini_chars.MinorNdisVersion = 0;
  } else {
    mini_chars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    mini_chars.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    mini_chars.MajorNdisVersion = 6;
    mini_chars.MinorNdisVersion = 1;
  }
  mini_chars.MajorDriverVersion = VENDOR_DRIVER_VERSION_MAJOR;
  mini_chars.MinorDriverVersion = VENDOR_DRIVER_VERSION_MINOR;

  FUNCTION_MSG("Driver MajorNdisVersion = %d, Driver MinorNdisVersion = %d\n", NDIS_MINIPORT_MAJOR_VERSION, NDIS_MINIPORT_MINOR_VERSION);
  FUNCTION_MSG("Windows MajorNdisVersion = %d, Windows MinorNdisVersion = %d\n", ndis_os_major_version, ndis_os_minor_version);

  mini_chars.Flags = NDIS_WDM_DRIVER;
  
  mini_chars.SetOptionsHandler = XenNet_SetOptions;
  mini_chars.InitializeHandlerEx = XenNet_Initialize;
  mini_chars.HaltHandlerEx = XenNet_Halt;
  mini_chars.UnloadHandler = XenNet_Unload;
  mini_chars.PauseHandler = XenNet_Pause;
  mini_chars.RestartHandler = XenNet_Restart;
  mini_chars.CheckForHangHandlerEx = XenNet_CheckForHang;
  mini_chars.ResetHandlerEx = XenNet_Reset;
  mini_chars.DevicePnPEventNotifyHandler = XenNet_DevicePnPEventNotify;
  mini_chars.ShutdownHandlerEx = XenNet_Shutdown;

  mini_chars.OidRequestHandler = XenNet_OidRequest;
  mini_chars.CancelOidRequestHandler = XenNet_CancelOidRequest;
  if (ndis_os_minor_version >= 1) {
    mini_chars.DirectOidRequestHandler = NULL;
    mini_chars.CancelDirectOidRequestHandler = NULL;
  }

  mini_chars.SendNetBufferListsHandler = XenNet_SendNetBufferLists;
  mini_chars.CancelSendHandler = XenNet_CancelSend;

  mini_chars.ReturnNetBufferListsHandler = XenNet_ReturnNetBufferLists;

  status = NdisMRegisterMiniportDriver(driver_object, registry_path, NULL, &mini_chars, &driver_handle);
  if (!NT_SUCCESS(status))
  {
    FUNCTION_MSG("NdisMRegisterMiniportDriver failed, status = 0x%x\n", status);
    return status;
  }

  FUNCTION_EXIT();

  return status;
}
