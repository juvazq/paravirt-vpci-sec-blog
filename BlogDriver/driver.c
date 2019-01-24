#include <wdm.h>
#include <vmbuskernelmodeclientlibapi.h>

#define OURS_POOL_TAG 'OURS'

typedef struct _FILTER_DEVICE_EXTENSION {
    IO_REMOVE_LOCK  RemoveLock;
    VMBCHANNEL      Channel;
} FILTER_DEVICE_EXTENSION, *PFILTER_DEVICE_EXTENSION;

NTSTATUS
HandlerAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
);

VOID
HandlerDriverUnload(
    _In_ PDRIVER_OBJECT  DriverObject
);

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
);

#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, HandlerAddDevice)
#pragma alloc_text(PAGE, HandlerDriverUnload)

NTSTATUS
HandlerAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
)
{
    PDEVICE_OBJECT              device = NULL;
    PFILTER_DEVICE_EXTENSION    extension = NULL;
    NTSTATUS                    status = STATUS_SUCCESS;

    PAGED_CODE();

    //
    // Create our filter device
    //
    status = IoCreateDevice(DriverObject,
        sizeof(FILTER_DEVICE_EXTENSION),
        NULL,
        FILE_DEVICE_BUS_EXTENDER,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &device);

    if (!NT_SUCCESS(status))
    {
        goto Cleanup;
    }

    //
    // Initialize our filter device extension
    //
    extension = (PFILTER_DEVICE_EXTENSION)device->DeviceExtension;
    RtlSecureZeroMemory(extension, sizeof(FILTER_DEVICE_EXTENSION));

    //
    // Attach the device to the VPCI FDO
    //
    PDEVICE_OBJECT attachedTo = IoAttachDeviceToDeviceStack(device, PhysicalDeviceObject->AttachedDevice);

    if (attachedTo == NULL)
    {
        status = STATUS_NO_SUCH_DEVICE;
        goto Cleanup;
    }

    //
    // Use the same flags than the VPCI FDO for the filter device
    //
    device->Flags |= attachedTo->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);

    //
    // Save a reference to the VPCI FDO VMBus channel
    // We know the offset thanks to the static analysis
    //
    extension->Channel = (VMBCHANNEL)((PULONG_PTR)(attachedTo->DeviceExtension))[3];

    //
    // Initialize the remove lock
    //
    IoInitializeRemoveLock(&extension->RemoveLock,
        OURS_POOL_TAG,
        0,
        0);

    //
    // Mark the filter device as ready to receive requests
    //
    device->Flags &= ~DO_DEVICE_INITIALIZING;    
    status = STATUS_SUCCESS;
    return status;

Cleanup:

    if (device != NULL)
    {
        IoDeleteDevice(device);
    }

    return status;
}

NTSTATUS
HandlerPass(
    _Inout_ PDEVICE_OBJECT  DeviceObject,
    _Inout_ PIRP            Irp
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
    PFILTER_DEVICE_EXTENSION extension = DeviceObject->DeviceExtension;
    UCHAR majorFunction = irpStack->MajorFunction;
    UCHAR minorFunction = irpStack->MinorFunction;

    //
    // Acquire the Remove Lock
    //
    status = IoAcquireRemoveLock(&extension->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
    {
        goto CompleteRequest;
    }

    if (majorFunction == IRP_MJ_PNP && minorFunction == IRP_MN_REMOVE_DEVICE)
    {
        //
        // Wait for requests to complete and release the remove lock
        //
        IoReleaseRemoveLockAndWait(&extension->RemoveLock, Irp);

        //
        // Pass the request down to the next device
        //
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(DeviceObject->DeviceObjectExtension->AttachedTo, Irp);

        //
        // Detach and delete our device
        //
        IoDetachDevice(DeviceObject->DeviceObjectExtension->AttachedTo);
        IoDeleteDevice(DeviceObject);
    }
    else
    {
        //
        // Pass the request down to the next device
        //
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(DeviceObject->DeviceObjectExtension->AttachedTo, Irp);

        //
        // Release Remove Lock
        //
        IoReleaseRemoveLock(&extension->RemoveLock, Irp);
    }

    return status;

CompleteRequest:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

VOID
HandlerDriverUnload(
    _In_ PDRIVER_OBJECT  DriverObject
)
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE();

    return;
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status = STATUS_SUCCESS;

    for (UINT32 uiIndex = 0; uiIndex <= IRP_MJ_MAXIMUM_FUNCTION; uiIndex++)
    {
        DriverObject->MajorFunction[uiIndex] = HandlerPass;
    }

    DriverObject->DriverExtension->AddDevice = HandlerAddDevice;
    DriverObject->DriverUnload = HandlerDriverUnload;

    return status;
}