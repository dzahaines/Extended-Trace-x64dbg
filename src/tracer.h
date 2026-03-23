#pragma once

#include "pluginmain.h"
#include <string>
#include <set>
#include <atomic>
#include <mutex>
#include <cstdio>

class Tracer
{
public:
    bool skipSystemModules = true;
    bool dumpTracedModules = true;
    bool logSkippedCalls = false;
    bool finishAtFirstStop = true;
    std::set<std::string> skipUserModules;
    std::string outputFolder;

    std::atomic<bool> running{ false };
    std::atomic<bool> paused{ false };
    std::atomic<bool> finished{ false };
    std::atomic<bool> stopRequested{ false };
    std::atomic<uint64_t> traceCounter{ 0 };
    std::atomic<ULONGLONG> traceStartTick{ 0 };
    std::atomic<ULONGLONG> traceEndTick{ 0 };

    enum SkipMode { SKIP_STEP_OVER = 0, SKIP_SILENT = 1 };
    SkipMode skipMode = SKIP_STEP_OVER;

    std::atomic<bool> needStepOver{ false };
    char stepOverType[16] = "";
    char stepOverTarget[512] = "";

    // Avoids expensive bridge calls on every traced instruction. Only accessed from the debug thread and script thread non-concurrently
    struct {
        duint base = 0;
        duint end = 0;
        bool isWhitelisted = false;
        bool isSystem = false;
        char name[MAX_MODULE_SIZE] = "";
    } moduleCache;

    // Signals TraceLoop that cbTraceStep has set info->stop=true
    std::atomic<bool> traceDone{ false };

    // CB_STEPPED sets stepDone when waitingForStep is true
    std::atomic<bool> waitingForStep{ false };
    std::atomic<bool> stepDone{ false };

    std::set<duint> tracedModuleBases;
    std::mutex tracedModulesMutex;
    duint lastTrackedBase = 0;

    void Start();
    void Stop();
    void OnBreakpoint();

    static void cbTraceStep(CBTYPE cbType, void* callbackInfo);
    static void cbStepped(CBTYPE cbType, void* callbackInfo);
    static void cbDebugEvent(CBTYPE cbType, void* callbackInfo);
    static void cbException(CBTYPE cbType, void* callbackInfo);

    static Tracer& Instance();

private:
    FILE* outFile = nullptr;
    std::mutex fileMutex;
    uint64_t flushCounter = 0;

    FILE* debugLogFile = nullptr;
    std::mutex debugLogMutex;
    void DebugLog(const char* fmt, ...);
    void OpenDebugLog();
    void CloseDebugLog();
    static std::string GetPluginDir();

    std::atomic<uint64_t> dbgStepOverCount{ 0 };

    void TraceLoop();
    static void ScriptEntry();

    void DumpTracedModules();
    void FlushIfNeeded();

    struct CallExtra {
        duint target = 0;
        char targetMod[MAX_MODULE_SIZE] = "";
        bool targetIsSystem = false;
        char func[512] = "";
    };

    void LogInstruction(duint cip, const BASIC_INSTRUCTION_INFO& instrInfo, const REGISTERCONTEXT_AVX512& regs, const CallExtra* callExtra = nullptr);
    void WriteLine(const char* line, size_t len);
    void BuildRegsJson(const REGISTERCONTEXT_AVX512& regs, std::string& out);

    std::string lineBuf;
    std::string regsBuf;

    std::string ReadStringAt(duint addr, int maxLen = 256);
    enum ModuleType { MOD_WHITELISTED, MOD_SYSTEM, MOD_SKIPPED, MOD_UNKNOWN };
    ModuleType ClassifyModuleCached(duint addr);

    bool IsSystemModule(duint addr);
    bool IsSkippedUserModule(duint addr, char* modNameOut);
    bool IsWhitelistedModule(duint addr);
    std::string JsonEscape(const char* s);
    std::string FormatRegValue(duint val);
};
