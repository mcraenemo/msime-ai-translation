#pragma once
#include "windows_ipc.h"

// Explicit test-only process startup, before any IPC threads or mappings exist.
inline void UseIsolatedTranslationProbeEndpoints()
{
    FANY_IME_SHARED_MEMORY = L"Local\\FanyImeSharedMemory-BufferProbe";
    FANY_IME_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeNamedPipe-BufferProbe";
    FANY_IME_TO_TSF_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeToTsfNamedPipe-BufferProbe";
    FANY_IME_TO_TSF_WORKER_THREAD_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeToTsfWorkerThreadNamedPipe-BufferProbe";
    FANY_IME_AUX_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeAuxNamedPipe-BufferProbe";
    FANY_IME_TSF_DIAGNOSTIC_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeTsfDiagnosticNamedPipe-BufferProbe";
    FANY_IME_VOICE_CONTROL_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeVoiceControlNamedPipe-BufferProbe";
    FANY_IME_STATS_PIPE_NAME_PREFIX = L"\\\\.\\pipe\\FanyImeStatsNamedPipe-BufferProbe-";
}
