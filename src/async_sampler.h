// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once


#include <pthread.h>
#include <array>
#include <signal.h>

#include "sampler.h"

class AsyncSampler : public Sampler
{
private:
    static AutoEvent s_threadSampledEvent;
    static AsyncSampler *s_instance;

    std::array<void *, 500> m_stackIPs;
    size_t m_numStackIPs;
    NativeThreadID m_stackThreadID;

    static void SignalHandler(int signal, siginfo_t *info, void *unused);

protected:
    virtual bool BeforeSampleAllThreads();
    virtual bool AfterSampleAllThreads();

    virtual bool SampleThread(ThreadID threadID);

public:
    static AsyncSampler *Instance()
    {
        return s_instance;
    }

    AsyncSampler(ICorProfilerInfo10* pProfInfo, CorProfiler *parent);
    virtual ~AsyncSampler() = default;
};
