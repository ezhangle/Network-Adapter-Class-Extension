// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Abstract:

    Contains the per-queue logic for NBL-to-NET_PACKET translation on the
    transmit path.

--*/

#include "NxXlatPrecomp.hpp"
#include "NxXlatCommon.hpp"
#include "NxTxXlat.tmh"

#include "NxTxXlat.hpp"
#include "NxPacketLayout.hpp"
#include "NxChecksumInfo.hpp"

static
void
NetClientQueueNotify(
    PVOID Queue
    )
{
    reinterpret_cast<NxTxXlat *>(Queue)->Notify();
}

static const NET_CLIENT_QUEUE_NOTIFY_DISPATCH QueueDispatch
{
    { sizeof(NET_CLIENT_QUEUE_NOTIFY_DISPATCH) },
    &NetClientQueueNotify
};

using PacketContext = NxNblTranslator::PacketContext;

_Use_decl_annotations_
NxTxXlat::NxTxXlat(
    size_t QueueId,
    NET_CLIENT_DISPATCH const * Dispatch,
    NET_CLIENT_ADAPTER Adapter,
    NET_CLIENT_ADAPTER_DISPATCH const * AdapterDispatch
    ) noexcept :
    m_queueId(QueueId),
    m_dispatch(Dispatch),
    m_adapter(Adapter),
    m_adapterDispatch(AdapterDispatch),
    m_contextBuffer(m_ringBuffer)
{
    m_adapterDispatch->GetProperties(m_adapter, &m_adapterProperties);
    m_adapterDispatch->GetDatapathCapabilities(m_adapter, &m_datapathCapabilities);
    m_nblDispatcher = static_cast<INxNblDispatcher *>(m_adapterProperties.NblDispatcher);

    m_shouldReportCounters = m_dispatch->NetClientQueryDriverConfigurationBoolean(TX_REPORT_PERF_COUNTERS);
    m_counterReportInterval = m_dispatch->NetClientQueryDriverConfigurationUlong(TX_PERF_COUNTERS_ITERATION_INTERVAL);
    m_shouldUpdateEcCounters = m_dispatch->NetClientQueryDriverConfigurationBoolean(EC_UPDATE_PERF_COUNTERS);

    if (m_shouldReportCounters)
    {
#ifdef _KERNEL_MODE
        LARGE_INTEGER timerDelay = {};
        KeInitializeTimer(&m_CounterReportTimer);
        KeInitializeDpc(&m_CounterReportDpc, NxTxXlat::CounterReportDpcRoutine, this);

        // in 100ns
        timerDelay.QuadPart = (-1 * MS_TO_100NS_CONVERSION * m_counterReportInterval);

        KeSetTimerEx(&
            m_CounterReportTimer,
            timerDelay,
            m_counterReportInterval,
            &m_CounterReportDpc);
#endif
    }
}

NxTxXlat::~NxTxXlat()
{
#ifdef _KERNEL_MODE
    if (m_shouldReportCounters)
    {
        KeCancelTimer(&m_CounterReportTimer);
        KeFlushQueuedDpcs();
    }
#endif

    // Waits until the EC completely exits
    m_executionContext.Terminate();

    if (m_ringBuffer.Get())
    {
        for (auto i = 0ul; i < m_ringBuffer.Count(); i++)
        {
            auto & context = m_contextBuffer.GetPacketContext<PacketContext>(i);

            context.~PacketContext();
        }
    }

    if (m_queue)
    {
        m_adapterDispatch->DestroyQueue(m_adapter, m_queue);
    }
}

_Use_decl_annotations_
size_t
NxTxXlat::GetQueueId(
    void
    ) const
{
    return m_queueId;
}

void
NxTxXlat::ArmNetBufferListArrivalNotification()
{
    m_queueNotification.Set();
}

void
NxTxXlat::ArmAdapterTxNotification()
{
    m_queueDispatch->SetArmed(m_queue, true);
}

NxTxXlat::ArmedNotifications
NxTxXlat::GetNotificationsToArm()
{
    ArmedNotifications notifications;

    // Shouldn't arm any notifications if we don't want to halt
    if (!m_producedPackets && !m_completedPackets)
    {
        // If the ringbuffer is not full (if there is room for OS to give more packets to NIC),
        // arm the translater serialization queue notification so that when new NBL is sent
        // to the translater later, it will wake up the transmit thread to send packets.
        notifications.Flags.ShouldArmNblArrival = !m_ringBuffer.AllPacketsOwnedByNic();

        // If 0 packets were completed by the Adapter in the last iteration, then arm
        // the adapter to issue Tx completion notifications.
        notifications.Flags.ShouldArmTxCompletion = m_ringBuffer.AnyNicPackets();

        // At least one notification should be set whenever going through this path.
        //
        // If the queue is not full, OS side notification will be armed, OS can wake translater
        // if it has more work. If NIC side notification is armed, NIC can wake translater if
        // it completed any packets.
        //
        // By default if this if block is called, the thread is going to halt
        // ShouldArmNblArrival or ShouldArmTxCompletion must be set. If there are
        // more work, this function is a no-op and keeps the main thread alive.
        NT_ASSERT(notifications.Value);
    }

    return notifications;
}

_Use_decl_annotations_
void
NxTxXlat::ArmNotifications(
    ArmedNotifications notifications
    )
{
    if (notifications.Flags.ShouldArmNblArrival)
    {
        ArmNetBufferListArrivalNotification();
    }

    if (notifications.Flags.ShouldArmTxCompletion)
    {
        ArmAdapterTxNotification();
    }
}

static EC_START_ROUTINE NetAdapterTransmitThread;

static
EC_RETURN
NetAdapterTransmitThread(
    PVOID StartContext
    )
{
    reinterpret_cast<NxTxXlat*>(StartContext)->TransmitThread();
    return EC_RETURN();
}

void
NxTxXlat::SetupTxThreadProperties()
{
#if _KERNEL_MODE
    // setup thread prioirty;
    ULONG threadPriority =
        m_dispatch->NetClientQueryDriverConfigurationUlong(TX_THREAD_PRIORITY);

    KeSetBasePriorityThread(KeGetCurrentThread(), threadPriority - (LOW_REALTIME_PRIORITY + LOW_PRIORITY)/2);

    BOOLEAN setThreadAffinity =
        m_dispatch->NetClientQueryDriverConfigurationBoolean(TX_THREAD_AFFINITY_ENABLED);

    ULONG threadAffinity =
        m_dispatch->NetClientQueryDriverConfigurationUlong(TX_THREAD_AFFINITY);

    if (setThreadAffinity != FALSE)
    {
        GROUP_AFFINITY Affinity = { 0 };
        GROUP_AFFINITY old;
        PROCESSOR_NUMBER CpuNum = { 0 };
        KeGetProcessorNumberFromIndex(threadAffinity, &CpuNum);
        Affinity.Group = CpuNum.Group;
        Affinity.Mask =
            (threadAffinity != THREAD_AFFINITY_NO_MASK) ?
            AFFINITY_MASK(CpuNum.Number) : ((ULONG_PTR)-1);
        KeSetSystemGroupAffinityThread(&Affinity, &old);
    }
#endif

}

void
NxTxXlat::TransmitThread()
{
    SetupTxThreadProperties();

    while (! m_executionContext.IsTerminated())
    {
        m_queueDispatch->Start(m_queue);

        auto cancelIssued = false;

        // This represents the core execution of the Tx path
        while (true)
        {
            // update NBL queue counters
            UpdateTxTranslationSpecificCounters();

            if (!cancelIssued)
            {
                // Check if the NBL serialization has any data
                PollNetBufferLists();

                // Post NBLs to the producer side of the NBL
                TranslateNbls();
            }

            // update ringbuffer counters;
            m_ringBuffer.UpdateRingbufferDepthCounters();

            // Allow the NetAdapter to return any packets that it is done with.
            YieldToNetAdapter();

            // Drain any packets that the NIC has completed.
            // This means returning the associated NBLs for each completed
            // NET_PACKET.
            DrainCompletions();

            if (m_shouldUpdateEcCounters)
            {
                // end of iteration, update execution context counters;
                m_executionContext.UpdateCounters(
                    !m_producedPackets && !m_completedPackets);
            }

            // Arms notifications if no forward progress was made in
            // this loop.
            WaitForWork();

            // This represents the wind down of Tx
            if (m_executionContext.IsStopping())
            {
                if (!cancelIssued)
                {
                    // Indicate cancellation to the adapter
                    // and drop all outstanding NBLs.
                    //
                    // One NBL may remain that has been partially programmed into the NIC.
                    // So that NBL is kept around until the end

                    m_queueDispatch->Cancel(m_queue);
                    DropQueuedNetBufferLists();

                    cancelIssued = true;
                }

                // The termination condition is that the NIC has returned all its
                // packets.
                if (!m_ringBuffer.AnyNicPackets())
                {
                    if (m_ringBuffer.AnyReturnedPackets())
                    {
                        DrainCompletions();
                        NT_ASSERT(!m_ringBuffer.AnyReturnedPackets());
                    }

                    // DropQueuedNetBufferLists had completed as many NBLs as possible, but there's
                    // a chance that one parital NBL couldn't be completed up there.  Do it now.
                    AbortNbls(m_currentNbl);
                    m_currentNbl = nullptr;
                    m_currentNetBuffer = nullptr;

                    m_queueDispatch->Stop(m_queue);
                    m_executionContext.SignalStopped();
                    break;
                }
            }
        }
    }
}

void
NxTxXlat::DrainCompletions()
{
    NxNblTranslator translator{ m_nblTranslationStats, *m_descriptor, m_datapathCapabilities, m_dmaAdapter.get(), m_contextBuffer, m_adapterProperties.MediaType };
    translator.m_netPacketLsoOffset = m_lsoOffset;

    auto const returned = m_ringBuffer.ReturnedPackets();
    auto const result = translator.CompletePackets(returned, m_bounceBufferPool);

    m_completedPackets = result.CompletedTo != returned.begin();

    if (result.CompletedChain)
    {
        m_nblDispatcher->SendNetBufferListsComplete(
            result.CompletedChain, result.NumCompletedNbls, 0);
    }

    UINT32 numberOfNewNetPakcetsCompleted = result.CompletedTo.GetDistanceFrom(returned.begin());
    NxRingBufferCounters delta = {};
    delta.NumberOfNetPacketsConsumed = numberOfNewNetPakcetsCompleted;

    m_ringBuffer.AdvanceNext(result.CompletedTo);
    m_ringBuffer.UpdateRingbufferPacketCounters(delta);
}

void
NxTxXlat::TranslateNbls()
{
    m_producedPackets = false;

    if (!m_currentNbl)
        return;

    NxNblTranslator translator{ m_nblTranslationStats, *m_descriptor, m_datapathCapabilities, m_dmaAdapter.get(), m_contextBuffer, m_adapterProperties.MediaType };
    translator.m_netPacketChecksumOffset = m_checksumOffset;
    translator.m_netPacketLsoOffset = m_lsoOffset;

    auto const availablePacketRange = m_ringBuffer.AvailablePackets();
    auto const nextUntranslatedPacket = translator.TranslateNbls(m_currentNbl, m_currentNetBuffer, availablePacketRange, m_bounceBufferPool);

    m_producedPackets = (nextUntranslatedPacket != availablePacketRange.begin());

    UINT32 numberOfNewNetPakcetsToSend = nextUntranslatedPacket.GetDistanceFrom(availablePacketRange.begin());
    NxRingBufferCounters delta = {};
    delta.NumberOfNetPacketsProduced = numberOfNewNetPakcetsToSend;

    m_ringBuffer.AdvanceEnd(nextUntranslatedPacket);
    m_ringBuffer.UpdateRingbufferPacketCounters(delta);
}

void
NxTxXlat::WaitForWork()
{
    auto notificationsToArm = GetNotificationsToArm();

    // In order to handle race conditions, the notifications that should
    // be armed at halt cannot change between the halt preparation and the
    // actual halt. If they do change, re-arm the necessary notifications
    // and loop again.
    if (notificationsToArm.Value != 0 && notificationsToArm.Value == m_lastArmedNotifications.Value)
    {
        m_executionContext.WaitForWork();

        // after halting, don't arm any notifications
        notificationsToArm.Value = 0;
    }

    ArmNotifications(notificationsToArm);

    m_lastArmedNotifications = notificationsToArm;
}

void
NxTxXlat::DropQueuedNetBufferLists()
{
    // This routine completes both the currently dequeued chain
    // of NBLs and the queued chain of NBLs with NDIS_STATUS_PAUSED.
    // This should only be run during the run down of the Tx path.
    // When the Tx path is being run down, no more NBLs are delivered
    // to the NBL queue.

    NT_ASSERT(m_executionContext.IsStopping());

    if (m_currentNbl)
    {
        if (m_currentNetBuffer == m_currentNbl->FirstNetBuffer)
        {
            AbortNbls(m_currentNbl);
            m_currentNbl = nullptr;
            m_currentNetBuffer = nullptr;
        }
        else
        {
            // At least one NB from the first NBL was already given to the NIC, so we
            // can't immediately complete it.  Complete the rest, at least.
            AbortNbls(m_currentNbl->Next);
            m_currentNbl->Next = nullptr;
        }
    }

    AbortNbls(DequeueNetBufferListQueue());
}

void
NxTxXlat::AbortNbls(
    _In_opt_ NET_BUFFER_LIST *nblChain)
{
    if (!nblChain)
        return;

    ndisSetStatusInNblChain(nblChain, NDIS_STATUS_PAUSED);

    m_nblDispatcher->SendNetBufferListsComplete(
        nblChain,
        ndisNumNblsInNblChain(nblChain),
        0);
}

void
NxTxXlat::PollNetBufferLists()
{
    // If the NBL chain is currently completely exhausted, go to the NBL
    // serialization queue to see if it has any new NBLs.
    //
    // m_currentNbl can be empty if either the last iteration of the translation
    // routine drained the current set of NBLs or if the last iteration
    // tried to dequeue from the serialized NBL forest and came up empty.
    if (!m_currentNbl)
    {
        m_currentNbl = DequeueNetBufferListQueue();
        if (m_currentNbl)
        {
            m_currentNetBuffer = m_currentNbl->FirstNetBuffer;
        }
    }
}

void
NxTxXlat::SendNetBufferLists(
    _In_ NET_BUFFER_LIST * NblChain,
    _In_ ULONG PortNumber,
    _In_ ULONG NumberOfNbls,
    _In_ ULONG SendFlags
    )
{
    UNREFERENCED_PARAMETER((PortNumber, NumberOfNbls, SendFlags));

    m_synchronizedNblQueue.Enqueue(NblChain);

    if (m_queueNotification.TestAndClear())
    {
        m_executionContext.SignalWork();
    }
}

PNET_BUFFER_LIST
NxTxXlat::DequeueNetBufferListQueue()
{
    return m_synchronizedNblQueue.DequeueAll();
}

void
NxTxXlat::YieldToNetAdapter()
{
    if (m_ringBuffer.AnyNicPackets())
    {
        if (m_dmaAdapter)
        {
            m_dmaAdapter->FlushIoBuffers(m_ringBuffer.NicPackets());
        }

        m_queueDispatch->Advance(m_queue);
    }
}

_Use_decl_annotations_
NTSTATUS
NxTxXlat::Initialize(
    void
    )
{
    m_adapterDispatch->GetDatapathCapabilities(m_adapter, &m_datapathCapabilities);

    NX_PERF_TX_NIC_CHARACTERISTICS perfCharacteristics = {};
    NX_PERF_TX_TUNING_PARAMETERS perfParameters;
    perfCharacteristics.Nic.IsDriverVerifierEnabled = !!m_adapterProperties.DriverIsVerifying;
    perfCharacteristics.Nic.MediaType = m_mediaType;
    perfCharacteristics.FragmentRingNumberOfElementsHint = m_datapathCapabilities.PreferredTxFragmentRingSize;
    perfCharacteristics.MaximumFragmentBufferSize = m_datapathCapabilities.MaximumTxFragmentSize;
    perfCharacteristics.NominalLinkSpeed = m_datapathCapabilities.NominalMaxTxLinkSpeed;
    perfCharacteristics.MaxPacketSizeWithLso = m_datapathCapabilities.MtuWithLso;

    NxPerfTunerCalculateTxParameters(&perfCharacteristics, &perfParameters);

    NET_CLIENT_QUEUE_CONFIG config;
    NET_CLIENT_QUEUE_CONFIG_INIT(
        &config,
        perfParameters.PacketRingElementCount,
        perfParameters.FragmentRingElementCount);

    Rtl::KArray<NET_CLIENT_PACKET_EXTENSION> addedPacketExtensions;

    CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
        PreparePacketExtensions(addedPacketExtensions),
        "Failed to add packet extensions to the RxQueue");

    if (addedPacketExtensions.count() != 0)
    {
        config.PacketExtensions = &addedPacketExtensions[0];
        config.NumberOfPacketExtensions = addedPacketExtensions.count();
    }

    CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
        m_adapterDispatch->CreateTxQueue(
            m_adapter,
            this,
            &QueueDispatch,
            &config,
            &m_queue,
            &m_queueDispatch),
        "Failed to create Tx queue. NxTxXlat=%p", this);

    // checksum offset
    m_checksumOffset = GetPacketExtensionOffsets(
        NET_PACKET_EXTENSION_CHECKSUM_NAME, NET_PACKET_EXTENSION_CHECKSUM_VERSION_1);

    m_lsoOffset = GetPacketExtensionOffsets(
        NET_PACKET_EXTENSION_LSO_NAME, NET_PACKET_EXTENSION_LSO_VERSION_1);

    m_descriptor = m_queueDispatch->GetNetDatapathDescriptor(m_queue);

    CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
        m_ringBuffer.Initialize(NET_DATAPATH_DESCRIPTOR_GET_PACKET_RING_BUFFER(m_descriptor)),
        "Failed to initialize packet ring buffer.");

    CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
        m_contextBuffer.Initialize(sizeof(PacketContext)),
        "Failed to initialize private context.");

    if (m_datapathCapabilities.TxMemoryConstraints.MappingRequirement == NET_CLIENT_MEMORY_MAPPING_REQUIREMENT_DMA_MAPPED)
    {
        m_dmaAdapter = wil::make_unique_nothrow<NxDmaAdapter>(m_datapathCapabilities, m_ringBuffer);

        if (!m_dmaAdapter)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        CX_RETURN_IF_NOT_NT_SUCCESS(m_dmaAdapter->Initialize(*m_dispatch));
    }

    CX_RETURN_IF_NOT_NT_SUCCESS(
        m_bounceBufferPool.Initialize(
            *m_dispatch,
            m_descriptor,
            m_datapathCapabilities,
            perfParameters.NumberOfBounceBuffers));

    for (auto i = 0ul; i < m_ringBuffer.Count(); i++)
    {
        new (&m_contextBuffer.GetPacketContext<PacketContext>(i)) PacketContext();
    }

    CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
        m_executionContext.Initialize(this, NetAdapterTransmitThread),
        "Failed to start Tx execution context. NxTxXlat=%p", this);

    m_executionContext.SetDebugNameHint(L"Transmit", GetQueueId(), m_adapterProperties.NetLuid);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void
NxTxXlat::Start(
    void
    )
{
    m_executionContext.Start();
}

_Use_decl_annotations_
void
NxTxXlat::Cancel(
    void
    )
{
    m_executionContext.Cancel();
}

_Use_decl_annotations_
void
NxTxXlat::Stop(
    void
    )
{
    m_executionContext.Stop();
}

size_t
NxTxXlat::GetPacketExtensionOffsets(
    PCWSTR ExtensionName,
    ULONG ExtensionVersion
    ) const
{
    NET_CLIENT_PACKET_EXTENSION extension = {};
    extension.Name = ExtensionName;
    extension.Version = ExtensionVersion;
    return m_queueDispatch->GetPacketExtensionOffset(m_queue, &extension);
}

NTSTATUS
NxTxXlat::PreparePacketExtensions(
    _Inout_ Rtl::KArray<NET_CLIENT_PACKET_EXTENSION>& addedPacketExtensions
    )
{
    // checksum
    NET_CLIENT_PACKET_EXTENSION extension = {};
    extension.Name = NET_PACKET_EXTENSION_CHECKSUM_NAME;
    extension.Version = NET_PACKET_EXTENSION_CHECKSUM_VERSION_1;

    //
    // Need to figure out how this would inter-op with capabilities interception
    // ideally here we should check both (Registered) && (Enabled) before we add
    // this extension to queue config.
    //
    if (NT_SUCCESS(m_adapterDispatch->QueryRegisteredPacketExtension(m_adapter, &extension)))
    {
        CX_RETURN_NTSTATUS_IF(
            STATUS_INSUFFICIENT_RESOURCES,
            !addedPacketExtensions.append(extension));
    }

    extension.Name = NET_PACKET_EXTENSION_LSO_NAME;
    extension.Version = NET_PACKET_EXTENSION_LSO_VERSION_1;

    if (NT_SUCCESS(m_adapterDispatch->QueryRegisteredPacketExtension(m_adapter, &extension)))
    {
        CX_RETURN_NTSTATUS_IF(
            STATUS_INSUFFICIENT_RESOURCES,
            !addedPacketExtensions.append(extension));
    }

    // more to come later!
    return STATUS_SUCCESS;
}

void
NxTxXlat::Notify()
{
    m_executionContext.SignalWork();
}

void
NxTxXlat::UpdateTxTranslationSpecificCounters()
{
    // Call per itertion
    m_IterationCountInLastInterval++;

    ULONG64 depth = m_synchronizedNblQueue.GetNblQueueDepth();
    m_CumulativeNBLQueueDepthInLastInterval += depth;

    if (depth == 0)
    {
        m_NBLQueueEmptyCount++;
    }
    else
    {
        m_NBLQueueOccupiedCount++;
    }
}

#ifdef _KERNEL_MODE
_Use_decl_annotations_
VOID
NxTxXlat::CounterReportDpcRoutine(
    _In_     struct _KDPC *Dpc,
    _In_opt_ PVOID        DeferredContext,
    _In_opt_ PVOID        SystemArgument1,
    _In_opt_ PVOID        SystemArgument2
    )
{
    UNREFERENCED_PARAMETER((Dpc, SystemArgument1, SystemArgument2));
    NxTxXlat* tx = static_cast<NxTxXlat*>(DeferredContext);
    tx->ReportCounters();
}
#endif

void
NxTxXlat::ReportCounters()
{
    NxRingBufferCounters localRBCounters;
    NxExecutionContextCounters localECCounters;

    localECCounters = m_executionContext.GetExecutionContextCounters();
    localRBCounters = m_ringBuffer.GetRingbufferCounters();
    m_ringBuffer.ResetRingbufferCounters();

    ULONG64 totalRingbufferStateSamples =
        localRBCounters.RingbufferEmptyCount +
        localRBCounters.RingbufferFullyOccupiedCount +
        localRBCounters.RingbufferPartiallyOccupiedCount;

    ULONG64 usefulIterationCount =
        localECCounters.IterationCount - localECCounters.BusyWaitIterationCount;

    TraceLoggingWrite(
        g_hNetAdapterCxXlatProvider,
        "TxTranslationCounterUpdates",
        TraceLoggingDescription("TX ETW performance counter event"),
        TraceLoggingUInt32(m_executionContext.GetExecutionContextIdentifier(), "threadID"),
        TraceLoggingUInt64(localRBCounters.IterationCountInLastInterval, "ringbufferUpdateIterationCount"),
        TraceLoggingUInt64(localRBCounters.CumulativeRingBufferDepthInLastInterval, "cumulativeRingBufferDepth"),
        TraceLoggingUInt64(totalRingbufferStateSamples, "totalNumberOfRingbufferStateSamples"),
        TraceLoggingUInt64(localRBCounters.RingbufferEmptyCount, "numberOfRingbufferEmptySamples"),
        TraceLoggingUInt64(localRBCounters.RingbufferFullyOccupiedCount, "numberOfRingbufferFullSamples"),
        TraceLoggingUInt64(localRBCounters.RingbufferPartiallyOccupiedCount, "numberOfRingbufferPartiallyUsedSamples"),
        TraceLoggingUInt64(localRBCounters.NumberOfNetPacketsProduced, "totalNumberOfNetpacketsProducedForReceive"),
        TraceLoggingUInt64(localRBCounters.NumberOfNetPacketsConsumed, "totalNumberOfNetpacketsCompletedReceiving"),
        TraceLoggingUInt64(localECCounters.IterationCount, "ecUpdateIterationCount"),
        TraceLoggingUInt64(localECCounters.BusyWaitIterationCount, "numberOfIterationsPolledWithNoPackets"),
        TraceLoggingUInt64(usefulIterationCount, "numberOfIterationsPolledWithPackets"),
        TraceLoggingUInt64(localECCounters.TotalCpuCycleTime, "totalNumberOfCpuCyclesRecord"),
        TraceLoggingUInt64(localECCounters.BusyWaitCycles, "numberOfCpuCyclesPolledWithNoPackets"),
        TraceLoggingUInt64(localECCounters.ProcessingCycles, "numberOfCpuCyclesSpentProcessingPackets"),
        TraceLoggingUInt64(localECCounters.IdleCycles, "numberOfCpuCyclesSleeping"),
        TraceLoggingUInt64(m_CumulativeNBLQueueDepthInLastInterval, "cumulativeNblQueueDepth"),
        TraceLoggingUInt64(m_NBLQueueEmptyCount + m_NBLQueueOccupiedCount, "numberOfNblQueueStateSamples"),
        TraceLoggingUInt64(m_NBLQueueEmptyCount, "numberOfEmptyNblQueueSamples"),
        TraceLoggingUInt64(m_NBLQueueOccupiedCount, "numberOfOccupiedNblQueueSamples")
    );

    m_CumulativeNBLQueueDepthInLastInterval = 0;
    m_IterationCountInLastInterval = 0;
    m_NBLQueueEmptyCount = 0;
    m_NBLQueueOccupiedCount = 0;
}

