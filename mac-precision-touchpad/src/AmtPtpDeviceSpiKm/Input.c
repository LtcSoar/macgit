#include "driver.h"
#include "Input.tmh"

VOID
AmtPtpSpiInputRoutineWorker(
	WDFDEVICE Device,
	WDFREQUEST PtpRequest
)
{
	NTSTATUS Status;
	PDEVICE_CONTEXT pDeviceContext;
	WDF_OBJECT_ATTRIBUTES Attributes;
	BOOLEAN RequestStatus = FALSE;
	WDFREQUEST SpiHidReadRequest;
	WDFMEMORY SpiHidReadOutputMemory;
	PWORKER_REQUEST_CONTEXT RequestContext;
	pDeviceContext = DeviceGetContext(Device);

	// This call is expected to happen after D0 entrance
	if (pDeviceContext->DeviceStatus == D3) {
		TraceEvents(
			TRACE_LEVEL_WARNING,
			TRACE_QUEUE,
			"%!FUNC! Unexpected call while device is in D3 status"
		);

		WdfRequestComplete(PtpRequest, STATUS_DEVICE_NOT_READY);
		return;
	}

	Status = WdfRequestForwardToIoQueue(
		PtpRequest,
		pDeviceContext->HidQueue
	);

	if (!NT_SUCCESS(Status)) {
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! WdfRequestForwardToIoQueue fails, status = %!STATUS!",
			Status
		);

		WdfRequestComplete(PtpRequest, Status);
		return;
	}

	// Late-init for the sleep workaround
	if (pDeviceContext->DeviceStatus == D0ActiveAndUnconfigured) {
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! Re-initialize device for sleep workaround"
		);

		Status = AmtPtpSpiSetState(
			Device,
			TRUE
		);

		if (!NT_SUCCESS(Status))
		{
			TraceEvents(
				TRACE_LEVEL_ERROR,
				TRACE_DRIVER,
				"%!FUNC! AmtPtpSpiSetState failed with %!STATUS!. Ignored anyway.",
				Status
			);
		}
		else {
			pDeviceContext->DeviceStatus = D0ActiveAndConfigured;
		}
	}

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WORKER_REQUEST_CONTEXT);
	Attributes.ParentObject = Device;

	Status = WdfRequestCreate(
		&Attributes,
		pDeviceContext->SpiTrackpadIoTarget,
		&SpiHidReadRequest
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! WdfRequestCreate fails, status = %!STATUS!",
			Status
		);

		return;
	}

	Status = WdfMemoryCreateFromLookaside(
		pDeviceContext->HidReadBufferLookaside,
		&SpiHidReadOutputMemory
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! WdfMemoryCreateFromLookaside fails, status = %!STATUS!",
			Status
		);

		WdfObjectDelete(SpiHidReadRequest);
		return;
	}

	// Assign context information
	RequestContext = WorkerRequestGetContext(SpiHidReadRequest);
	RequestContext->DeviceContext = pDeviceContext;
	RequestContext->RequestMemory = SpiHidReadOutputMemory;

	// Invoke HID read request to the device.
	Status = WdfIoTargetFormatRequestForInternalIoctl(
		pDeviceContext->SpiTrackpadIoTarget,
		SpiHidReadRequest,
		IOCTL_HID_READ_REPORT,
		NULL,
		0,
		SpiHidReadOutputMemory,
		0
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! WdfIoTargetFormatRequestForInternalIoctl fails, status = %!STATUS!",
			Status
		);

		WdfObjectDelete(SpiHidReadOutputMemory);
		WdfObjectDelete(SpiHidReadRequest);
		return;
	}

	WdfRequestSetCompletionRoutine(
		SpiHidReadRequest,
		AmtPtpRequestCompletionRoutine,
		RequestContext
	);

	RequestStatus = WdfRequestSend(
		SpiHidReadRequest,
		pDeviceContext->SpiTrackpadIoTarget,
		NULL
	);

	if (!RequestStatus)
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! AmtPtpSpiInputRoutineWorker request failed to sent"
		);

		WdfObjectDelete(SpiHidReadOutputMemory);
		WdfObjectDelete(SpiHidReadRequest);
	}
}

VOID
AmtPtpRequestCompletionRoutine(
	WDFREQUEST SpiRequest,
	WDFIOTARGET Target,
	PWDF_REQUEST_COMPLETION_PARAMS Params,
	WDFCONTEXT Context
)
{
	NTSTATUS Status;
	PWORKER_REQUEST_CONTEXT RequestContext;
	PDEVICE_CONTEXT pDeviceContext;

	LONG SpiRequestLength;
	PSPI_TRACKPAD_PACKET pSpiTrackpadPacket;

	WDFREQUEST PtpRequest;
	PTP_REPORT PtpReport;
	WDFMEMORY PtpRequestMemory;

	LARGE_INTEGER CurrentCounter;
	LONGLONG CounterDelta;
	BOOLEAN SessionEnded = TRUE;

	UNREFERENCED_PARAMETER(Target);

	// Get context
	RequestContext = (PWORKER_REQUEST_CONTEXT) Context;
	pDeviceContext = RequestContext->DeviceContext;

	// Read report and fulfill PTP request.
	// If no report is found, just exit.
	Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->HidQueue, &PtpRequest);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfIoQueueRetrieveNextRequest failed with %!STATUS!",
			Status
		);

		goto cleanup;
	}

	SpiRequestLength = (LONG) WdfRequestGetInformation(SpiRequest);
	pSpiTrackpadPacket = (PSPI_TRACKPAD_PACKET) WdfMemoryGetBuffer(Params->Parameters.Ioctl.Output.Buffer, NULL);

	// Safe measurement for buffer overrun
	if (SpiRequestLength < 46) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! Input too small: %d < 46",
			SpiRequestLength
		);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	// Get Counter
	KeQueryPerformanceCounter(
		&CurrentCounter
	);

	CounterDelta = (CurrentCounter.QuadPart - pDeviceContext->LastReportTime.QuadPart) / 100;
	pDeviceContext->LastReportTime.QuadPart = CurrentCounter.QuadPart;

	// Write report
	PtpReport.ReportID = REPORTID_MULTITOUCH;
	PtpReport.ContactCount = pSpiTrackpadPacket->NumOfFingers;
	PtpReport.IsButtonClicked = pSpiTrackpadPacket->ClickOccurred;

	UINT8 AdjustedCount = (pSpiTrackpadPacket->NumOfFingers > 5) ? 5 : pSpiTrackpadPacket->NumOfFingers;
	for (UINT8 Count = 0; Count < AdjustedCount; Count++)
	{
		PtpReport.Contacts[Count].ContactID = Count;
		PtpReport.Contacts[Count].X = ((pSpiTrackpadPacket->Fingers[Count].X - pDeviceContext->TrackpadInfo.XMin) > 0) ? 
			(USHORT)(pSpiTrackpadPacket->Fingers[Count].X - pDeviceContext->TrackpadInfo.XMin) : 0;
		PtpReport.Contacts[Count].Y = ((pDeviceContext->TrackpadInfo.YMax - pSpiTrackpadPacket->Fingers[Count].Y) > 0) ? 
			(USHORT)(pDeviceContext->TrackpadInfo.YMax - pSpiTrackpadPacket->Fingers[Count].Y) : 0;
		PtpReport.Contacts[Count].TipSwitch = (pSpiTrackpadPacket->Fingers[Count].Pressure > 0) ? 1 : 0;

		// $S = \pi * (Touch_{Major} * Touch_{Minor}) / 4$
		// $S = \pi * r^2$
		// $r^2 = (Touch_{Major} * Touch_{Minor}) / 4$
		// Using i386 in 2018 is evil
		PtpReport.Contacts[Count].Confidence = (pSpiTrackpadPacket->Fingers[Count].TouchMajor < 2500 &&
			pSpiTrackpadPacket->Fingers[Count].TouchMinor < 2500) ? 1 : 0;

		if (!SessionEnded && PtpReport.Contacts[Count].TipSwitch) SessionEnded = FALSE;

		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_HID_INPUT,
			"%!FUNC! PTP Contact %d OX %d, OY %d, X %d, Y %d",
			Count,
			pSpiTrackpadPacket->Fingers[Count].OriginalX,
			pSpiTrackpadPacket->Fingers[Count].OriginalY,
			pSpiTrackpadPacket->Fingers[Count].X,
			pSpiTrackpadPacket->Fingers[Count].Y
		);
	}

	if (SessionEnded)
	{
		for (UINT8 i = 0; i < MAPPING_MAX; i++)
		{
			pDeviceContext->PtpMapping[i].ContactID = -1;
			pDeviceContext->PtpMapping[i].OriginalX = -1;
			pDeviceContext->PtpMapping[i].OriginalY = -1;
		}
	}

	if (CounterDelta >= 0xFF)
	{
		PtpReport.ScanTime = 0xFF;
	}
	else
	{
		PtpReport.ScanTime = (USHORT) CounterDelta;
	}

	Status = WdfRequestRetrieveOutputMemory(
		PtpRequest,
		&PtpRequestMemory
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfRequestRetrieveOutputBuffer failed with %!STATUS!",
			Status
		);

		goto exit;
	}

	Status = WdfMemoryCopyFromBuffer(
		PtpRequestMemory,
		0,
		(PVOID) &PtpReport,
		sizeof(PTP_REPORT)
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfMemoryCopyFromBuffer failed with %!STATUS!",
			Status
		);

		goto exit;
	}

	// Set information
	WdfRequestSetInformation(
		PtpRequest,
		sizeof(PTP_REPORT)
	);

exit:
	WdfRequestComplete(
		PtpRequest,
		Status
	);

cleanup:
	// Clean up
	pSpiTrackpadPacket = NULL;
	WdfObjectDelete(SpiRequest);
	WdfObjectDelete(RequestContext->RequestMemory);
}
