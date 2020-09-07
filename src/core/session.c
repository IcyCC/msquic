/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    A "session" manages TLS session state, which is used for session
    resumption across connections. On Windows it also manages silo
    and network compartment state.

--*/

#include "precomp.h"
#ifdef QUIC_CLOG
#include "session.c.clog.h"
#endif

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicSessionAlloc(
    _In_opt_ QUIC_REGISTRATION* Registration,
    _In_opt_ void* Context,
    _Outptr_ _At_(*NewSession, __drv_allocatesMem(Mem))
        QUIC_SESSION** NewSession
    )
{
    QUIC_SESSION* Session = QUIC_ALLOC_NONPAGED(sizeof(QUIC_SESSION));
    if (Session == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "session" ,
            sizeof(QUIC_SESSION));
        return QUIC_STATUS_OUT_OF_MEMORY;
    }

    QuicZeroMemory(Session, sizeof(QUIC_SESSION));
    Session->Type = QUIC_HANDLE_TYPE_SESSION;
    Session->ClientContext = Context;

    if (Registration != NULL) {
        Session->Registration = Registration;

#ifdef QUIC_SILO
        Session->Silo = QuicSiloGetCurrentServer();
        QuicSiloAddRef(Session->Silo);
#endif

#ifdef QUIC_COMPARTMENT_ID
        Session->CompartmentId = QuicCompartmentIdGetCurrent();
#endif
    }

    QuicTraceEvent(
        SessionCreated,
        "[sess][%p] Created, Registration=%p",
        Session,
        Session->Registration);

    QuicRundownInitialize(&Session->Rundown);
    QuicRwLockInitialize(&Session->ServerCacheLock);
    QuicDispatchLockInitialize(&Session->ConnectionsLock);
    QuicListInitializeHead(&Session->Connections);

    *NewSession = Session;

    return QUIC_STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
MsQuicSessionFree(
    _In_ _Pre_defensive_ __drv_freesMem(Mem)
        QUIC_SESSION* Session
    )
{
    //
    // If you hit this assert, you are trying to clean up a session without
    // first cleaning up all the child connections first.
    //
    QUIC_TEL_ASSERT(QuicListIsEmpty(&Session->Connections));
    QuicRundownUninitialize(&Session->Rundown);

    QuicDispatchLockUninitialize(&Session->ConnectionsLock);
    QuicRwLockUninitialize(&Session->ServerCacheLock);
    QuicTraceEvent(
        SessionDestroyed,
        "[sess][%p] Destroyed",
        Session);
    QUIC_FREE(Session);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QUIC_API
MsQuicSessionOpen(
    _In_ _Pre_defensive_ HQUIC RegistrationContext,
    _In_opt_ void* Context,
    _Outptr_ _At_(*NewSession, __drv_allocatesMem(Mem)) _Pre_defensive_
        HQUIC *NewSession
    )
{
    QUIC_STATUS Status;
    QUIC_SESSION* Session = NULL;

    QuicTraceEvent(
        ApiEnter,
        "[ api] Enter %u (%p).",
        QUIC_TRACE_API_SESSION_OPEN,
        RegistrationContext);

    if (RegistrationContext == NULL ||
        RegistrationContext->Type != QUIC_HANDLE_TYPE_REGISTRATION ||
        NewSession == NULL) {
        Status = QUIC_STATUS_INVALID_PARAMETER;
        goto Error;
    }

    Status =
        QuicSessionAlloc(
            (QUIC_REGISTRATION*)RegistrationContext,
            Context,
            &Session);
    if (QUIC_FAILED(Status)) {
        goto Error;
    }

    QuicLockAcquire(&Session->Registration->SessionLock);
    QuicListInsertTail(&Session->Registration->Sessions, &Session->Link);
    QuicLockRelease(&Session->Registration->SessionLock);

    *NewSession = (HQUIC)Session;
    Session = NULL;

Error:

    if (Session != NULL) {
        MsQuicSessionFree(Session);
    }

    QuicTraceEvent(
        ApiExitStatus,
        "[ api] Exit %u",
        Status);

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QUIC_API
MsQuicSessionClose(
    _In_ _Pre_defensive_ __drv_freesMem(Mem)
        HQUIC Handle
    )
{
    if (Handle == NULL) {
        return;
    }

    QUIC_TEL_ASSERT(Handle->Type == QUIC_HANDLE_TYPE_SESSION);
    _Analysis_assume_(Handle->Type == QUIC_HANDLE_TYPE_SESSION);
    if (Handle->Type != QUIC_HANDLE_TYPE_SESSION) {
        return;
    }

    QuicTraceEvent(
        ApiEnter,
        "[ api] Enter %u (%p).",
        QUIC_TRACE_API_SESSION_CLOSE,
        Handle);

#pragma prefast(suppress: __WARNING_25024, "Pointer cast already validated.")
    QUIC_SESSION* Session = (QUIC_SESSION*)Handle;

    QuicTraceEvent(
        SessionCleanup,
        "[sess][%p] Cleaning up",
        Session);

    if (Session->Registration != NULL) {
        QuicLockAcquire(&Session->Registration->SessionLock);
        QuicListEntryRemove(&Session->Link);
        QuicLockRelease(&Session->Registration->SessionLock);
    } else {
        //
        // This is the global unregistered session. All connections need to be
        // immediately cleaned up. Use shutdown to ensure this all gets placed
        // on the worker queue.
        //
        MsQuicSessionShutdown(Handle, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
    }

    QuicRundownReleaseAndWait(&Session->Rundown);
    MsQuicSessionFree(Session);

    QuicTraceEvent(
        ApiExit,
        "[ api] Exit");
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QUIC_API
MsQuicSessionShutdown(
    _In_ _Pre_defensive_ HQUIC Handle,
    _In_ QUIC_CONNECTION_SHUTDOWN_FLAGS Flags,
    _In_ _Pre_defensive_ QUIC_UINT62 ErrorCode
    )
{
    QUIC_DBG_ASSERT(Handle != NULL);
    QUIC_DBG_ASSERT(Handle->Type == QUIC_HANDLE_TYPE_SESSION);

    if (ErrorCode > QUIC_UINT62_MAX) {
        return;
    }

    QuicTraceEvent(
        ApiEnter,
        "[ api] Enter %u (%p).",
        QUIC_TRACE_API_SESSION_SHUTDOWN,
        Handle);

    if (Handle && Handle->Type == QUIC_HANDLE_TYPE_SESSION) {
#pragma prefast(suppress: __WARNING_25024, "Pointer cast already validated.")
        QUIC_SESSION* Session = (QUIC_SESSION*)Handle;

        QuicTraceEvent(
            SessionShutdown,
            "[sess][%p] Shutting down connections, Flags=%u, ErrorCode=%llu",
            Session,
            Flags,
            ErrorCode);

        QuicDispatchLockAcquire(&Session->ConnectionsLock);

        QUIC_LIST_ENTRY* Entry = Session->Connections.Flink;
        while (Entry != &Session->Connections) {

            QUIC_CONNECTION* Connection =
                QUIC_CONTAINING_RECORD(Entry, QUIC_CONNECTION, SessionLink);

            if (InterlockedCompareExchange16(
                    (short*)&Connection->BackUpOperUsed, 1, 0) == 0) {

                QUIC_OPERATION* Oper = &Connection->BackUpOper;
                Oper->FreeAfterProcess = FALSE;
                Oper->Type = QUIC_OPER_TYPE_API_CALL;
                Oper->API_CALL.Context = &Connection->BackupApiContext;
                Oper->API_CALL.Context->Type = QUIC_API_TYPE_CONN_SHUTDOWN;
                Oper->API_CALL.Context->CONN_SHUTDOWN.Flags = Flags;
                Oper->API_CALL.Context->CONN_SHUTDOWN.ErrorCode = ErrorCode;
                QuicConnQueueHighestPriorityOper(Connection, Oper);
            }

            Entry = Entry->Flink;
        }

        QuicDispatchLockRelease(&Session->ConnectionsLock);
    }

    QuicTraceEvent(
        ApiExit,
        "[ api] Exit");
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSessionTraceRundown(
    _In_ QUIC_SESSION* Session
    )
{
    QuicTraceEvent(
        SessionRundown,
        "[sess][%p] Rundown, Registration=%p", // TODO - Fix manifest
        Session,
        Session->Registration);

    QuicDispatchLockAcquire(&Session->ConnectionsLock);

    for (QUIC_LIST_ENTRY* Link = Session->Connections.Flink;
        Link != &Session->Connections;
        Link = Link->Flink) {
        QuicConnQueueTraceRundown(
            QUIC_CONTAINING_RECORD(Link, QUIC_CONNECTION, SessionLink));
    }

    QuicDispatchLockRelease(&Session->ConnectionsLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSessionRegisterConnection(
    _Inout_ QUIC_SESSION* Session,
    _Inout_ QUIC_CONNECTION* Connection
    )
{
    QuicSessionUnregisterConnection(Connection);
    Connection->Session = Session;

    if (Session->Registration != NULL) {
        Connection->Registration = Session->Registration;
        QuicRundownAcquire(&Session->Registration->ConnectionRundown);
#ifdef QuicVerifierEnabledByAddr
        Connection->State.IsVerifying = Session->Registration->IsVerifying;
#endif
        QuicConnApplySettings(Connection, &Session->Settings);
    }

    QuicTraceEvent(
        ConnRegisterSession,
        "[conn][%p] Registered with session: %p",
        Connection,
        Session);
    BOOLEAN Success = QuicRundownAcquire(&Session->Rundown);
    QUIC_DBG_ASSERT(Success); UNREFERENCED_PARAMETER(Success);
    QuicDispatchLockAcquire(&Session->ConnectionsLock);
    QuicListInsertTail(&Session->Connections, &Connection->SessionLink);
    QuicDispatchLockRelease(&Session->ConnectionsLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicSessionUnregisterConnection(
    _Inout_ QUIC_CONNECTION* Connection
    )
{
    if (Connection->Session == NULL) {
        return;
    }
    QUIC_SESSION* Session = Connection->Session;
    Connection->Session = NULL;
    QuicTraceEvent(
        ConnUnregisterSession,
        "[conn][%p] Unregistered from session: %p",
        Connection,
        Session);
    QuicDispatchLockAcquire(&Session->ConnectionsLock);
    QuicListEntryRemove(&Connection->SessionLink);
    QuicDispatchLockRelease(&Session->ConnectionsLock);
    QuicRundownRelease(&Session->Rundown);
}

//
// Requires Session->Lock to be held (shared or exclusive).
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_SERVER_CACHE*
QuicSessionServerCacheLookup(
    _In_ QUIC_SESSION* Session,
    _In_ uint16_t ServerNameLength,
    _In_reads_(ServerNameLength)
        const char* ServerName,
    _In_ uint32_t Hash
    )
{
    QUIC_HASHTABLE_LOOKUP_CONTEXT Context;
    QUIC_HASHTABLE_ENTRY* Entry =
        QuicHashtableLookup(&Session->ServerCache, Hash, &Context);

    while (Entry != NULL) {
        QUIC_SERVER_CACHE* Temp =
            QUIC_CONTAINING_RECORD(Entry, QUIC_SERVER_CACHE, Entry);
        if (Temp->ServerNameLength == ServerNameLength &&
            memcmp(Temp->ServerName, ServerName, ServerNameLength) == 0) {
            return Temp;
        }
        Entry = QuicHashtableLookupNext(&Session->ServerCache, &Context);
    }

    return NULL;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Success_(return != FALSE)
BOOLEAN
QuicSessionServerCacheGetState(
    _In_ QUIC_SESSION* Session,
    _In_z_ const char* ServerName,
    _Out_ uint32_t* QuicVersion,
    _Out_ QUIC_TRANSPORT_PARAMETERS* Parameters,
    _Out_ QUIC_SEC_CONFIG** ClientSecConfig
    )
{
    uint16_t ServerNameLength = (uint16_t)strlen(ServerName);
    uint32_t Hash = QuicHashSimple(ServerNameLength, (const uint8_t*)ServerName);

    QuicRwLockAcquireShared(&Session->ServerCacheLock);

    QUIC_SERVER_CACHE* Cache =
        QuicSessionServerCacheLookup(
            Session,
            ServerNameLength,
            ServerName,
            Hash);

    if (Cache != NULL) {
        *QuicVersion = Cache->QuicVersion;
        *Parameters = Cache->TransportParameters;
        if (Cache->SecConfig != NULL) {
            *ClientSecConfig = QuicTlsSecConfigAddRef(Cache->SecConfig);
        } else {
            *ClientSecConfig = NULL;
        }
    }

    QuicRwLockReleaseShared(&Session->ServerCacheLock);

    return Cache != NULL;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSessionServerCacheSetStateInternal(
    _In_ QUIC_SESSION* Session,
    _In_ uint16_t ServerNameLength,
    _In_reads_(ServerNameLength)
        const char* ServerName,
    _In_ uint32_t QuicVersion,
    _In_ const QUIC_TRANSPORT_PARAMETERS* Parameters,
    _In_opt_ QUIC_SEC_CONFIG* SecConfig
    )
{
    uint32_t Hash = QuicHashSimple(ServerNameLength, (const uint8_t*)ServerName);

    QuicRwLockAcquireExclusive(&Session->ServerCacheLock);

    QUIC_SERVER_CACHE* Cache =
        QuicSessionServerCacheLookup(
            Session,
            ServerNameLength,
            ServerName,
            Hash);

    if (Cache != NULL) {
        Cache->QuicVersion = QuicVersion;
        Cache->TransportParameters = *Parameters;
        if (SecConfig != NULL) {
            if (Cache->SecConfig != NULL) {
                QuicTlsSecConfigRelease(Cache->SecConfig);
            }
            Cache->SecConfig = QuicTlsSecConfigAddRef(SecConfig);
        }

    } else {
#pragma prefast(suppress: __WARNING_6014, "Memory is correctly freed (MsQuicSessionClose).")
        Cache = QUIC_ALLOC_PAGED(sizeof(QUIC_SERVER_CACHE) + ServerNameLength);

        if (Cache != NULL) {
            memcpy(Cache + 1, ServerName, ServerNameLength);
            Cache->ServerName = (const char*)(Cache + 1);
            Cache->ServerNameLength = ServerNameLength;
            Cache->QuicVersion = QuicVersion;
            Cache->TransportParameters = *Parameters;
            if (SecConfig != NULL) {
                Cache->SecConfig = QuicTlsSecConfigAddRef(SecConfig);
            }

            QuicHashtableInsert(&Session->ServerCache, &Cache->Entry, Hash, NULL);

        } else {
            QuicTraceEvent(
                AllocFailure,
                "Allocation of '%s' failed. (%llu bytes)",
                "server cache entry",
                sizeof(QUIC_SERVER_CACHE) + ServerNameLength);
        }
    }

    QuicRwLockReleaseExclusive(&Session->ServerCacheLock);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicSessionServerCacheSetState(
    _In_ QUIC_SESSION* Session,
    _In_z_ const char* ServerName,
    _In_ uint32_t QuicVersion,
    _In_ const QUIC_TRANSPORT_PARAMETERS* Parameters,
    _In_ QUIC_SEC_CONFIG* SecConfig
    )
{
    QuicSessionServerCacheSetStateInternal(
        Session,
        (uint16_t)strlen(ServerName),
        ServerName,
        QuicVersion,
        Parameters,
        SecConfig);
}
