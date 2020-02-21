#pragma once

#include <thread>
#include <mutex>
#include <functional>
#include <condition_variable>
#include <string>

class CorProfiler;

class ManualEvent
{
private:
    std::mutex m_mtx;
    std::condition_variable m_cv;
    bool m_set = false;

    static void DoNothing()
    {

    }

public:
    ManualEvent() = default;
    ~ManualEvent() = default;
    ManualEvent(ManualEvent& other) = delete;
    ManualEvent(ManualEvent&& other) = delete;
    ManualEvent& operator= (ManualEvent& other) = delete;
    ManualEvent& operator= (ManualEvent&& other) = delete;

    void Wait(std::function<void()> spuriousCallback = DoNothing)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        while (!m_set)
        {
            m_cv.wait(lock, [&]() { return m_set; });
            if (!m_set)
            {
                spuriousCallback();
            }
        }
    }

    void Signal()
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_set = true;
    }

    void Reset()
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_set = false;
    }
};

class Sampler
{
private:
    static Sampler* s_instance;

    std::thread m_workerThread;
    static ManualEvent s_waitEvent;

    ICorProfilerInfo10* corProfilerInfo;

    static void DoSampling(ICorProfilerInfo10* pProfInfo, CorProfiler *parent);

    std::wstring GetClassName(ClassID classId);
    std::wstring GetModuleName(ModuleID modId);
    std::wstring GetFunctionName(FunctionID funcID, const COR_PRF_FRAME_INFO frameInfo);
public:
    static Sampler* Instance()
    {
        return s_instance;
    }

    Sampler(ICorProfilerInfo10* pProfInfo, CorProfiler *parent);
    ~Sampler() = default;

    void Start();
    void Stop();

    HRESULT StackSnapshotCallback(FunctionID funcId,
        UINT_PTR ip,
        COR_PRF_FRAME_INFO frameInfo,
        ULONG32 contextSize,
        BYTE context[],
        void* clientData);
};
