/*
 * usb_comms.c
 * usb:ds 双接口实现,仅保留 5.0+ 路径 (参考 SysDVR UsbComms.c)
 * 接口 0 = 串流, 接口 1 = 文件传输
 */
#include <string.h>
#include "usb_link.h"
#include "../../protocol/swlink_protocol.h"

typedef struct {
    RwLock lock;
    RwLock lock_in;   /* IN 端点(写)串行化 */
    RwLock lock_out;  /* OUT 端点(读)串行化 */
    bool initialized;

    UsbDsInterface* interface;
    UsbDsEndpoint* endpoint_in;
    UsbDsEndpoint* endpoint_out;
} UsbCommsInterface;

static bool g_initialized = false;
static UsbCommsInterface g_interfaces[USB_NUM_INTERFACES];
static RwLock g_global_lock;

/* PostBufferAsync 要求 0x1000 对齐,非对齐数据经这些中转 buffer */
static alignas(0x1000) u8 g_ep_in_buf[USB_NUM_INTERFACES][0x1000];
static alignas(0x1000) u8 g_ep_out_buf[USB_NUM_INTERFACES][0x1000];

static Result _interfaceInit5x(u32 intf_ind)
{
    Result rc = 0;
    UsbCommsInterface* interface = &g_interfaces[intf_ind];

    struct usb_interface_descriptor interface_descriptor = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 0, /* 注册后由 interface_index 填充 */
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
    };

    struct usb_endpoint_descriptor endpoint_descriptor_in = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x200,
    };

    struct usb_endpoint_descriptor endpoint_descriptor_out = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x40,
    };

    struct usb_ss_endpoint_companion_descriptor endpoint_companion = {
        .bLength = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst = 0x0F,
        .bmAttributes = 0x00,
        .wBytesPerInterval = 0x00,
    };

    interface->initialized = 1;

    memset(g_ep_in_buf[intf_ind], 0, 0x1000);
    memset(g_ep_out_buf[intf_ind], 0, 0x1000);

    rc = usbDsRegisterInterface(&interface->interface);
    if (R_FAILED(rc)) return rc;

    interface_descriptor.bInterfaceNumber = interface->interface->interface_index;
    endpoint_descriptor_in.bEndpointAddress += interface_descriptor.bInterfaceNumber + 1;
    endpoint_descriptor_out.bEndpointAddress += interface_descriptor.bInterfaceNumber + 1;

    /* Full Speed (USB 1.1) */
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Full, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Full, &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Full, &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;

    /* High Speed (USB 2.0) */
    endpoint_descriptor_in.wMaxPacketSize = 0x200;
    endpoint_descriptor_out.wMaxPacketSize = 0x200;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_High, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_High, &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_High, &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;

    /* Super Speed (USB 3.0) */
    endpoint_descriptor_in.wMaxPacketSize = 0x400;
    endpoint_descriptor_out.wMaxPacketSize = 0x400;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_RegisterEndpoint(interface->interface, &interface->endpoint_in, endpoint_descriptor_in.bEndpointAddress);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_RegisterEndpoint(interface->interface, &interface->endpoint_out, endpoint_descriptor_out.bEndpointAddress);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_EnableInterface(interface->interface);
    return rc;
}

Result swlinkUsbInitialize(void)
{
    Result rc = 0;
    rwlockWriteLock(&g_global_lock);

    if (g_initialized) {
        rwlockWriteUnlock(&g_global_lock);
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }

    rc = usbDsInitialize();

    if (R_SUCCEEDED(rc)) {
        u8 iManufacturer, iProduct, iSerialNumber;
        static const u16 supported_langs[1] = { 0x0409 };
        rc = usbDsAddUsbLanguageStringDescriptor(NULL, supported_langs, 1);
        if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iManufacturer, "SwLink");
        if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iProduct, "SwLink Switch");
        if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iSerialNumber, "swlink-01");

        struct usb_device_descriptor device_descriptor = {
            .bLength = USB_DT_DEVICE_SIZE,
            .bDescriptorType = USB_DT_DEVICE,
            .bcdUSB = 0x0110,
            .bDeviceClass = 0x00,
            .bDeviceSubClass = 0x00,
            .bDeviceProtocol = 0x00,
            .bMaxPacketSize0 = 0x40,
            .idVendor = SWLINK_USB_VID,
            .idProduct = SWLINK_USB_PID,
            .bcdDevice = 0x0100,
            .iManufacturer = iManufacturer,
            .iProduct = iProduct,
            .iSerialNumber = iSerialNumber,
            .bNumConfigurations = 0x01
        };
        if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device_descriptor);

        device_descriptor.bcdUSB = 0x0200;
        if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device_descriptor);

        device_descriptor.bcdUSB = 0x0300;
        device_descriptor.bMaxPacketSize0 = 0x09;
        if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device_descriptor);

        /* Binary Object Store: 声明 USB2.0 扩展 + USB3.0 能力 */
        u8 bos[0x16] = {
            0x05, USB_DT_BOS,
            0x16, 0x00,
            0x02,
            0x07, USB_DT_DEVICE_CAPABILITY, 0x02,
            0x02, 0x00, 0x00, 0x00,
            0x0A, USB_DT_DEVICE_CAPABILITY, 0x03,
            0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00
        };
        if (R_SUCCEEDED(rc)) rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
    }

    if (R_SUCCEEDED(rc)) {
        for (u32 i = 0; i < USB_NUM_INTERFACES; i++) {
            rc = _interfaceInit5x(i);
            if (R_FAILED(rc)) break;
        }
    }

    if (R_SUCCEEDED(rc)) rc = usbDsEnable();

    if (R_SUCCEEDED(rc)) {
        g_initialized = true;
    }

    rwlockWriteUnlock(&g_global_lock);

    if (R_FAILED(rc)) swlinkUsbExit();

    return rc;
}

void swlinkUsbExit(void)
{
    rwlockWriteLock(&g_global_lock);

    for (u32 i = 0; i < USB_NUM_INTERFACES; i++) {
        UsbCommsInterface* interface = &g_interfaces[i];
        usbDsInterface_Close(interface->interface);
        interface->initialized = false;
        interface->endpoint_in = NULL;
        interface->endpoint_out = NULL;
        interface->interface = NULL;
    }

    usbDsExit();
    g_initialized = false;

    rwlockWriteUnlock(&g_global_lock);
}

static Result _read(UsbCommsInterface* interface, u8* stage_buf, void* buffer, size_t size, size_t* transferredSize, u64 timeout)
{
    Result rc = 0;
    u32 urbId = 0;
    u8* bufptr = (u8*)buffer;
    u8* transfer_buffer = NULL;
    u32 chunksize = 0;
    u32 tmp_transferredSize = 0;
    size_t total_transferredSize = 0;
    UsbDsReportData reportdata;
    bool via_stage;

    rc = usbDsWaitReady(timeout);
    if (R_FAILED(rc)) return rc;

    while (size) {
        if (((u64)bufptr) & 0xfff) {
            /* 目标地址未页对齐:先收到中转 buffer,再 memcpy */
            transfer_buffer = stage_buf;
            memset(stage_buf, 0, 0x1000);

            chunksize = 0x1000;
            chunksize -= ((u64)bufptr) & 0xfff;
            if (size < chunksize) chunksize = size;
            via_stage = true;
        } else {
            transfer_buffer = bufptr;
            chunksize = size;
            via_stage = false;
        }

        rc = usbDsEndpoint_PostBufferAsync(interface->endpoint_out, transfer_buffer, chunksize, &urbId);
        if (R_FAILED(rc)) return rc;

        if (R_FAILED(eventWait(&interface->endpoint_out->CompletionEvent, timeout)))
            usbDsEndpoint_Cancel(interface->endpoint_out);

        eventWait(&interface->endpoint_out->CompletionEvent, 3E+9);
        eventClear(&interface->endpoint_out->CompletionEvent);

        rc = usbDsEndpoint_GetReportData(interface->endpoint_out, &reportdata);
        if (R_FAILED(rc)) return rc;

        rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferredSize);
        if (R_FAILED(rc)) return rc;

        if (tmp_transferredSize > chunksize) tmp_transferredSize = chunksize;
        total_transferredSize += (size_t)tmp_transferredSize;

        if (via_stage) memcpy(bufptr, transfer_buffer, tmp_transferredSize);
        bufptr += tmp_transferredSize;
        size -= tmp_transferredSize;

        if (tmp_transferredSize < chunksize) break;
    }

    if (transferredSize) *transferredSize = total_transferredSize;
    return rc;
}

static Result _write(UsbCommsInterface* interface, u8* stage_buf, const void* buffer, size_t size, size_t* transferredSize, u64 timeout)
{
    Result rc = 0;
    u32 urbId = 0;
    u32 chunksize = 0;
    u8* bufptr = (u8*)buffer;
    u8* transfer_buffer = NULL;
    u32 tmp_transferredSize = 0;
    size_t total_transferredSize = 0;
    UsbDsReportData reportdata;

    rc = usbDsWaitReady(timeout);
    if (R_FAILED(rc)) return rc;

    while (size) {
        if (((u64)bufptr) & 0xfff) {
            /* 源地址未页对齐:先拷贝进中转 buffer */
            transfer_buffer = stage_buf;
            memset(stage_buf, 0, 0x1000);

            chunksize = 0x1000;
            chunksize -= ((u64)bufptr) & 0xfff;
            if (size < chunksize) chunksize = size;

            memcpy(stage_buf, bufptr, chunksize);
        } else {
            transfer_buffer = bufptr;
            chunksize = size;
        }

        rc = usbDsEndpoint_PostBufferAsync(interface->endpoint_in, transfer_buffer, chunksize, &urbId);
        if (R_FAILED(rc)) return rc;

        if (R_FAILED(eventWait(&interface->endpoint_in->CompletionEvent, timeout)))
            usbDsEndpoint_Cancel(interface->endpoint_in);

        eventWait(&interface->endpoint_in->CompletionEvent, 3E+9);
        eventClear(&interface->endpoint_in->CompletionEvent);

        rc = usbDsEndpoint_GetReportData(interface->endpoint_in, &reportdata);
        if (R_FAILED(rc)) return rc;

        rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferredSize);
        if (R_FAILED(rc)) return rc;

        if (tmp_transferredSize > chunksize) tmp_transferredSize = chunksize;
        total_transferredSize += (size_t)tmp_transferredSize;

        bufptr += tmp_transferredSize;
        size -= tmp_transferredSize;

        if (tmp_transferredSize < chunksize) break;
    }

    if (transferredSize) *transferredSize = total_transferredSize;
    return rc;
}

size_t swlinkUsbRead(u32 interface, void* buffer, size_t size, u64 timeout)
{
    size_t transferredSize = 0;
    u32 state = 0;
    Result rc, rc2;
    UsbCommsInterface* inter;
    bool initialized;

    if (interface >= USB_NUM_INTERFACES) return 0;
    inter = &g_interfaces[interface];

    rwlockReadLock(&inter->lock);
    initialized = inter->initialized;
    rwlockReadUnlock(&inter->lock);
    if (!initialized) return 0;

    rwlockWriteLock(&inter->lock_out);
    rc = _read(inter, g_ep_out_buf[interface], buffer, size, &transferredSize, timeout);
    rwlockWriteUnlock(&inter->lock_out);

    if (R_FAILED(rc) && rc != KernelError_TimedOut) {
        rc2 = usbDsGetState(&state);
        if (R_SUCCEEDED(rc2) && state != UsbState_Configured) {
            /* 传输途中状态变化(拔插),重试一次 */
            rwlockWriteLock(&inter->lock_out);
            rc = _read(inter, g_ep_out_buf[interface], buffer, size, &transferredSize, timeout);
            rwlockWriteUnlock(&inter->lock_out);
        }
    }
    return transferredSize;
}

size_t swlinkUsbWrite(u32 interface, const void* buffer, size_t size, u64 timeout)
{
    size_t transferredSize = 0;
    u32 state = 0;
    Result rc, rc2;
    UsbCommsInterface* inter;
    bool initialized;

    if (interface >= USB_NUM_INTERFACES) return 0;
    inter = &g_interfaces[interface];

    rwlockReadLock(&inter->lock);
    initialized = inter->initialized;
    rwlockReadUnlock(&inter->lock);
    if (!initialized) return 0;

    rwlockWriteLock(&inter->lock_in);
    rc = _write(inter, g_ep_in_buf[interface], buffer, size, &transferredSize, timeout);
    rwlockWriteUnlock(&inter->lock_in);

    if (R_FAILED(rc) && rc != KernelError_TimedOut) {
        rc2 = usbDsGetState(&state);
        if (R_SUCCEEDED(rc2) && state != UsbState_Configured) {
            rwlockWriteLock(&inter->lock_in);
            rc = _write(inter, g_ep_in_buf[interface], buffer, size, &transferredSize, timeout);
            rwlockWriteUnlock(&inter->lock_in);
        }
    }
    return transferredSize;
}
