// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <cstdio>

#include "CorProfiler.h"
#include "sampler.h"

using std::ifstream;
using std::string;

ThreadState Sampler::GetThreadState(ThreadID threadID)
{
    NativeThreadID nativeThreadID = GetNativeThreadID(threadID);

    // TODO: single character file access is probably not the best for performance
    string fileName = "/proc/self/task/" + std::to_string((uintptr_t)nativeThreadID) + "/stat";
    fprintf(m_outputFile, "Parsing contents of %s for thread\n", fileName.c_str());
    ifstream threadState(fileName);

    char ch;
    bool sawCloseParen = false;
    while (threadState.get(ch) && threadState.good())
    {
        if (sawCloseParen && ch != ' ')
        {
            break;
        }

        if (ch == ')')
        {
            sawCloseParen = true;
        }
    }

    fprintf(m_outputFile, "thread state char=%c\n", ch);

    // TODO: should verify that this is completely trustworthy. I did some basic investigation and it seems
    // to map, i.e. any threads marked as ThreadState::Suspended seemed to be actually suspended.
    switch(ch)
    {
        case 'R': // Running
            return ThreadState::Running;

        case 'I': // Idle
        case 'W': // Paging or waking
        case 'K': // Wakekill
            // return ThreadState::Transition;
        case 'D': // Waiting on disk
        case 'P': // Parked
        case 'S': // Sleeping in a wait
        case 't': // Tracing/debugging
        case 'T': // Stopped on a signal
            return ThreadState::Suspended;

        case 'x': // dead
        case 'X': // Dead
        case 'Z': // Zombie
            return ThreadState::Dead;

        default:
            fprintf(m_outputFile, "Saw invalid character in thread state %c\n", ch);
            return ThreadState::Running;
    }

    return ThreadState::Running;
}

NativeThreadID Sampler::GetCurrentNativeThreadID()
{
    return gettid();
}

void *Sampler::GetCurrentThreadStackBase()
{
    bool success;
    pthread_attr_t attrs;
    if( pthread_getattr_np( pthread_self(), &attrs ) == 0 )
    {
        void   *stackAddr;
        size_t  stackSize;
        if( pthread_attr_getstack( &attrs, &stackAddr, &stackSize ) == 0 )
        {
            fprintf(m_outputFile, "Got stack stackAddr=%p stackSize=%zu\n", stackAddr, stackSize);
            return (void *)((uintptr_t)stackAddr + (stackSize - 1));
        }
    }

    return nullptr;
}
