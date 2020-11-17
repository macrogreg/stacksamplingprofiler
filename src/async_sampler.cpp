// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "CorProfiler.h"
#include "async_sampler.h"

#include <signal.h>
#include <cinttypes>
#include <locale>
#include <codecvt>
#include <libunwind.h>
#include <execinfo.h>

using std::wstring_convert;
using std::codecvt_utf8;
using std::string;

AutoEvent AsyncSampler::s_threadSampledEvent;
AsyncSampler *AsyncSampler::s_instance;

static HRESULT __stdcall DoStackSnapshotStackSnapShotCallbackWrapper(
    FunctionID funcId,
    UINT_PTR ip,
    COR_PRF_FRAME_INFO frameInfo,
    ULONG32 contextSize,
    BYTE context[],
    void* clientData)
{
    return AsyncSampler::Instance()->StackSnapshotCallback(funcId,
        ip,
        frameInfo,
        contextSize,
        context,
        clientData);
}

//static
void AsyncSampler::SignalHandler(int signal, siginfo_t *info, void *unused)
{
    uint64_t tid;
    pthread_t threadID = pthread_self();
    pthread_threadid_np(threadID, &tid);

    printf("Signal %d caught, running on thread %" PRIx64 "\n", signal, tid);

    void *stackaddr = pthread_get_stackaddr_np(threadID);
    size_t stacksize = pthread_get_stacksize_np(threadID);

    printf("stackaddr=%p stacksize=%zu thread=%" PRIx64 "\n", stackaddr, stacksize, tid);

    ICorProfilerInfo10 *pProfilerInfo = s_instance->pCorProfilerInfo;
    constexpr size_t numAddresses = 100;
    void *addresses[numAddresses];
    int backtraceSize = backtrace(addresses, numAddresses);
    printf("backtrace returned size %d\n", backtraceSize);
    for (int i = 0; i < backtraceSize; ++i)
    {
        // Managed name
        FunctionID functionID;
        HRESULT hr = pProfilerInfo->GetFunctionFromIP((uint8_t *)addresses[i], &functionID);
        if (hr != S_OK)
        {
            printf("Unknown native frame ip=0x%p\n", addresses[i]);
            continue;
        }

        WSTRING functionName = s_instance->GetFunctionName(functionID, NULL);

#if WIN32
        wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
#else // WIN32
        wstring_convert<codecvt_utf8<char16_t>, char16_t> convert;
#endif // WIN32

        string printable = convert.to_bytes(functionName);
        printf("    %s (funcId=0x%" PRIx64 ")\n", printable.c_str(), (uint64_t)functionID);
    }

    unw_context_t context;
    unw_cursor_t cursor;
    unw_getcontext(&context);
    unw_init_local(&cursor, &context);

    int result = 0;
    do
    {
        // unw_word_t sp;
        unw_word_t ip;
        // unw_get_reg(&cursor, UNW_REG_SP, &sp);
        unw_get_reg(&cursor, UNW_REG_IP, &ip);

        char symbolName[256];
        unw_word_t offset;
        if (unw_get_proc_name(&cursor, symbolName, sizeof(symbolName), &offset) == 0)
        {
            printf(" (%s+0x%lx)\n", symbolName, offset);
        }
        else
        {
            // TODO: this never gets hit. It stops unwinding at the _sigtramp

            // Managed name
            FunctionID functionID;
            HRESULT hr = pProfilerInfo->GetFunctionFromIP((uint8_t *)ip, &functionID);
            if (hr != S_OK)
            {
                printf("GetFunctionFromIP failed with hr=0x%x\n", hr);
                continue;
            }

            WSTRING functionName = s_instance->GetFunctionName(functionID, NULL);

#if WIN32
            wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
#else // WIN32
            wstring_convert<codecvt_utf8<char16_t>, char16_t> convert;
#endif // WIN32

            string printable = convert.to_bytes(functionName);
            printf("    %s (funcId=0x%" PRIx64 ")\n", printable.c_str(), (uint64_t)functionID);
        }
    } while ((result = unw_step(&cursor)) > 0);

    printf("Done walking stack with libunwind, result=%d\n", result);

    ThreadID profThreadID;
    HRESULT hr = pProfilerInfo->GetCurrentThreadID(&profThreadID);
    if (FAILED(hr))
    {
        printf("GetCurrentThreadID failed with hr=0x%x\n", hr);
    }

    hr = pProfilerInfo->DoStackSnapshot(profThreadID,
                                        DoStackSnapshotStackSnapShotCallbackWrapper,
                                        COR_PRF_SNAPSHOT_REGISTER_CONTEXT,
                                        NULL,
                                        NULL,
                                        0);
    if (FAILED(hr))
    {
        if (hr == E_FAIL)
        {
            printf("Managed thread id=0x%" PRIx64 " has no managed frames to walk \n", (uint64_t)profThreadID);
        }
        else
        {
            printf("DoStackSnapshot for thread id=0x%" PRIx64 " failed with hr=0x%x \n", (uint64_t)profThreadID, hr);
        }
    }

    s_threadSampledEvent.Signal();
}

bool AsyncSampler::BeforeSampleAllThreads()
{
    return true;
}

bool AsyncSampler::AfterSampleAllThreads()
{
    return true;
}

bool AsyncSampler::SampleThread(ThreadID threadID)
{
    auto it = threadIDMap.find(threadID);
    if (it == threadIDMap.end())
    {
        assert(!"Unexpected thread found");
        return false;
    }

    pthread_t nativeThreadId = it->second;

    uint64_t tid;
    pthread_threadid_np(nativeThreadId, &tid);
    printf("Sending signal to thread %" PRIx64 "!\n", tid);
    int result = pthread_kill(nativeThreadId, SIGUSR2) != 0;
    printf("pthread_kill result=%d\n", result);

    // Only sample one thread at a time
    s_threadSampledEvent.Wait();

    return true;
}

AsyncSampler::AsyncSampler(ICorProfilerInfo10* pProfInfo, CorProfiler *parent) :
    Sampler(pProfInfo, parent),
    threadIDMap()
{
    s_instance = this;

    struct sigaction sampleAction;
    sampleAction.sa_flags = 0;
    // sampleAction.sa_handler = AsyncSampler::SignalHandler;
    sampleAction.sa_sigaction = AsyncSampler::SignalHandler;
    sampleAction.sa_flags |= SA_SIGINFO;
    sigemptyset(&sampleAction.sa_mask);
    sigaddset(&sampleAction.sa_mask, SIGUSR2);

    struct sigaction oldAction;
    int result = sigaction(SIGUSR2, &sampleAction, &oldAction);
    printf("sigaction result=%d\n", result);
}

void AsyncSampler::ThreadCreated(uintptr_t threadId)
{
    pthread_t tid = pthread_self();
    threadIDMap.insertNew(threadId, tid);
}

void AsyncSampler::ThreadDestroyed(uintptr_t threadId)
{
    // should probably delete it from the map
}

HRESULT AsyncSampler::StackSnapshotCallback(FunctionID funcId, UINT_PTR ip, COR_PRF_FRAME_INFO frameInfo, ULONG32 contextSize, BYTE context[], void* clientData)
{
    WSTRING functionName = GetFunctionName(funcId, frameInfo);

#if WIN32
    wstring_convert<codecvt_utf8<wchar_t>, wchar_t> convert;
#else // WIN32
    wstring_convert<codecvt_utf8<char16_t>, char16_t> convert;
#endif // WIN32

    string printable = convert.to_bytes(functionName);
    printf("    %s (funcId=0x%" PRIx64 ")\n", printable.c_str(), (uint64_t)funcId);
    return S_OK;
}
