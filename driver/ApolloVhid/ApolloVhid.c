#include "ApolloVhid.h"

// Keyboard (Report ID 1) + relative mouse (Report ID 2).
// Mouse axes are signed 16-bit relative values. Vertical wheel and AC Pan are signed 8-bit.
static UCHAR ApolloVhidReportDescriptor[] = {
    // Keyboard
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, APOLLO_VHID_KEYBOARD_REPORT_ID,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x01,
    0x19, 0x00,
    0x29, 0xFF,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x06,
    0x81, 0x00,
    0xC0,

    // Relative mouse
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x85, APOLLO_VHID_MOUSE_REPORT_ID,
    0x09, 0x01,
    0xA1, 0x00,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x05,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x05,
    0x81, 0x02,
    0x75, 0x03,
    0x95, 0x01,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x16, 0x00, 0x80,
    0x26, 0xFF, 0x7F,
    0x75, 0x10,
    0x95, 0x02,
    0x81, 0x06,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,
    0x05, 0x0C,
    0x0A, 0x38, 0x02,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,
    0xC0,
    0xC0
};

static NTSTATUS ApolloVhidSubmit(
    _In_ PDEVICE_CONTEXT Context,
    _In_ UCHAR ReportId,
    _In_reads_bytes_(ReportLength) PUCHAR Report,
    _In_ ULONG ReportLength)
{
    HID_XFER_PACKET packet;

    if (Context->VhfHandle == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    RtlZeroMemory(&packet, sizeof(packet));
    packet.reportBuffer = Report;
    packet.reportBufferLen = ReportLength;
    packet.reportId = ReportId;

    return VhfReadReportSubmit(Context->VhfHandle, &packet);
}

static VOID ApolloVhidReleaseAll(_In_ PDEVICE_CONTEXT Context)
{
    APOLLO_VHID_KEYBOARD_REPORT keyboard = {0};
    APOLLO_VHID_MOUSE_REPORT mouse = {0};

    if (Context->VhfHandle == NULL) {
        return;
    }

    (void)ApolloVhidSubmit(Context,
                          APOLLO_VHID_KEYBOARD_REPORT_ID,
                          (PUCHAR)&keyboard,
                          sizeof(keyboard));
    (void)ApolloVhidSubmit(Context,
                          APOLLO_VHID_MOUSE_REPORT_ID,
                          (PUCHAR)&mouse,
                          sizeof(mouse));
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, ApolloVhidEvtDeviceAdd);
    return WdfDriverCreate(DriverObject,
                           RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES,
                           &config,
                           WDF_NO_HANDLE);
}

NTSTATUS ApolloVhidEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_FILEOBJECT_CONFIG fileConfig;
    PDEVICE_CONTEXT context;
    VHF_CONFIG vhfConfig;
    DECLARE_CONST_UNICODE_STRING(deviceName, APOLLO_VHID_NT_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(symbolicLinkName, APOLLO_VHID_DOS_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
                               WDF_NO_EVENT_CALLBACK,
                               WDF_NO_EVENT_CALLBACK,
                               ApolloVhidEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = ApolloVhidEvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    context = ApolloVhidGetContext(device);
    context->VhfHandle = NULL;

    status = WdfDeviceCreateSymbolicLink(device, &symbolicLinkName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = ApolloVhidEvtIoDeviceControl;

    status = WdfIoQueueCreate(device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    VHF_CONFIG_INIT(&vhfConfig,
                    WdfDeviceWdmGetDeviceObject(device),
                    (USHORT)sizeof(ApolloVhidReportDescriptor),
                    ApolloVhidReportDescriptor);

    vhfConfig.VendorID = 0xF055;
    vhfConfig.ProductID = 0xA110;
    vhfConfig.VersionNumber = 0x0001;

    status = VhfCreate(&vhfConfig, &context->VhfHandle);
    if (!NT_SUCCESS(status)) {
        context->VhfHandle = NULL;
        return status;
    }

    status = VhfStart(context->VhfHandle);
    if (!NT_SUCCESS(status)) {
        VhfDelete(context->VhfHandle, TRUE);
        context->VhfHandle = NULL;
        return status;
    }

    return STATUS_SUCCESS;
}

VOID ApolloVhidEvtDeviceCleanup(_In_ WDFOBJECT DeviceObject)
{
    PDEVICE_CONTEXT context = ApolloVhidGetContext(DeviceObject);

    PAGED_CODE();

    if (context->VhfHandle != NULL) {
        ApolloVhidReleaseAll(context);
        VhfDelete(context->VhfHandle, TRUE);
        context->VhfHandle = NULL;
    }
}

VOID ApolloVhidEvtFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    PDEVICE_CONTEXT context = ApolloVhidGetContext(device);

    ApolloVhidReleaseAll(context);
}

VOID ApolloVhidEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT context = ApolloVhidGetContext(device);
    PVOID buffer = NULL;
    size_t bufferLength = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_APOLLO_VHID_KEYBOARD:
        status = WdfRequestRetrieveInputBuffer(Request,
                                               sizeof(APOLLO_VHID_KEYBOARD_REPORT),
                                               &buffer,
                                               &bufferLength);
        if (NT_SUCCESS(status)) {
            status = ApolloVhidSubmit(context,
                                     APOLLO_VHID_KEYBOARD_REPORT_ID,
                                     (PUCHAR)buffer,
                                     (ULONG)sizeof(APOLLO_VHID_KEYBOARD_REPORT));
        }
        break;

    case IOCTL_APOLLO_VHID_MOUSE:
        status = WdfRequestRetrieveInputBuffer(Request,
                                               sizeof(APOLLO_VHID_MOUSE_REPORT),
                                               &buffer,
                                               &bufferLength);
        if (NT_SUCCESS(status)) {
            status = ApolloVhidSubmit(context,
                                     APOLLO_VHID_MOUSE_REPORT_ID,
                                     (PUCHAR)buffer,
                                     (ULONG)sizeof(APOLLO_VHID_MOUSE_REPORT));
        }
        break;

    default:
        break;
    }

    WdfRequestComplete(Request, status);
}
