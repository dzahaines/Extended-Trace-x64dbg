#include "tracer.h"
#include "pedumper.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <direct.h>
#include <cstdarg>
#include <ctime>

Tracer& Tracer::Instance()
{
    static Tracer instance;
    return instance;
}

static std::string ToLower(const char* s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

std::string Tracer::GetPluginDir()
{
    char dir[MAX_PATH] = "";
    HMODULE hMod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetPluginDir, &hMod);
    GetModuleFileNameA(hMod, dir, MAX_PATH);

    char* slash = strrchr(dir, '\\');
    if (slash)
        *(slash + 1) = '\0';
    else
        dir[0] = '\0';

    return std::string(dir);
}

void Tracer::OpenDebugLog()
{
    std::lock_guard<std::mutex> lock(debugLogMutex);
    if (debugLogFile)
        return;

    std::string logPath = GetPluginDir() + "ExtendedTrace.log";
    debugLogFile = fopen(logPath.c_str(), "a");
    if (debugLogFile)
    {
        setvbuf(debugLogFile, nullptr, _IOLBF, 4096);
        time_t now = time(nullptr);
        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(debugLogFile, "\n========== Trace session started: %s ==========\n", timeBuf);
        fflush(debugLogFile);
    }
}

void Tracer::CloseDebugLog()
{
    std::lock_guard<std::mutex> lock(debugLogMutex);
    if (debugLogFile)
    {
        time_t now = time(nullptr);
        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(debugLogFile, "========== Trace session ended: %s ==========\n\n", timeBuf);
        fflush(debugLogFile);
        fclose(debugLogFile);
        debugLogFile = nullptr;
    }
}

void Tracer::DebugLog(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    dputs(buf);

    std::lock_guard<std::mutex> lock(debugLogMutex);
    if (debugLogFile)
    {
        DWORD tick = GetTickCount();
        fprintf(debugLogFile, "[%lu] %s\n", tick, buf);
    }
}

void Tracer::Start()
{
    if (running.load())
    {
        dputs("Trace is already running!");
        return;
    }

    if (paused.load())
    {
        if (!outFile)
        {
            dputs("Cannot resume: trace file was closed.");
            paused.store(false);
            return;
        }

        if (!DbgIsDebugging())
        {
            dputs("No process is being debugged!");
            return;
        }

        stopRequested.store(false);
        needStepOver.store(false);
        traceDone.store(false);
        waitingForStep.store(false);
        stepDone.store(false);
        moduleCache.base = 0;
        running.store(true);
        paused.store(false);

        DebugLog("[TRACE] Trace resumed, continuing from instruction %llu",
            (unsigned long long)traceCounter.load());
        _plugin_startscript(ScriptEntry);
        return;
    }

    if (outputFolder.empty())
    {
        dputs("Output folder is not set!");
        return;
    }

    if (!DbgIsDebugging())
    {
        dputs("No process is being debugged!");
        return;
    }

    _mkdir(outputFolder.c_str());

    std::string traceFile = outputFolder;
    if (traceFile.back() != '\\' && traceFile.back() != '/')
        traceFile += '\\';
    traceFile += "trace.jsonl";

    outFile = fopen(traceFile.c_str(), "wb");
    if (!outFile)
    {
        dprintf("Failed to open output file: %s\n", traceFile.c_str());
        return;
    }
    setvbuf(outFile, nullptr, _IOFBF, 1024 * 1024);

    traceCounter.store(0);
    stopRequested.store(false);
    needStepOver.store(false);
    traceDone.store(false);
    waitingForStep.store(false);
    stepDone.store(false);
    running.store(true);
    paused.store(false);
    finished.store(false);
    traceStartTick.store(GetTickCount64());
    traceEndTick.store(0);
    flushCounter = 0;
    moduleCache.base = 0;
    lastTrackedBase = 0;

    lineBuf.reserve(4096);
    regsBuf.reserve(2048);

    {
        std::lock_guard<std::mutex> lock(tracedModulesMutex);
        tracedModuleBases.clear();
    }

    OpenDebugLog();
    DebugLog("[TRACE] Trace started, folder: %s", outputFolder.c_str());
    _plugin_startscript(ScriptEntry);
}

void Tracer::Stop()
{
    if (paused.load())
    {
        DebugLog("[TRACE] Stop() called in paused state");
        paused.store(false);

        {
            std::lock_guard<std::mutex> lock(fileMutex);
            if (outFile)
            {
                fflush(outFile);
                fclose(outFile);
                outFile = nullptr;
            }
        }

        traceEndTick.store(GetTickCount64());

        DebugLog("[TRACE] Trace stopped from paused state. %llu total instructions.",
            (unsigned long long)traceCounter.load());
        CloseDebugLog();

        if (dumpTracedModules && DbgIsDebugging())
        {
            dputs("Dumping traced user modules...");
            DumpTracedModules();
        }
        finished.store(true);
        return;
    }

    if (!running.load())
        return;

    stopRequested.store(true);
    dputs("Trace stop requested...");
}

void Tracer::OnBreakpoint()
{
}

void Tracer::cbStepped(CBTYPE, void* callbackInfo)
{
    Tracer& t = Tracer::Instance();
    if (t.waitingForStep.load())
        t.stepDone.store(true);
}

void Tracer::ScriptEntry()
{
    Tracer::Instance().TraceLoop();
}

void Tracer::cbDebugEvent(CBTYPE, void* callbackInfo)
{
}

void Tracer::cbException(CBTYPE, void* callbackInfo)
{
}

void Tracer::DumpTracedModules()
{
    if (!DbgIsDebugging())
    {
        dputs("Cannot dump modules: debugger not active.");
        return;
    }

    std::set<duint> bases;
    {
        std::lock_guard<std::mutex> lock(tracedModulesMutex);
        bases = tracedModuleBases;
    }

    if (bases.empty())
    {
        dputs("No user modules were traced, nothing to dump.");
        return;
    }

    std::string dumpDir = outputFolder;
    if (dumpDir.back() != '\\' && dumpDir.back() != '/')
        dumpDir += '\\';
    dumpDir += "modules";
    _mkdir(dumpDir.c_str());

    int dumped = 0, failed = 0;

    for (duint base : bases)
    {
        char modName[MAX_MODULE_SIZE] = "";
        if (!Script::Module::NameFromAddr(base, modName))
            snprintf(modName, sizeof(modName), "0x%llX", (unsigned long long)base);

        char modPath[MAX_PATH] = "";
        Script::Module::PathFromAddr(base, modPath);

        const char* ext = ".dll";
        const char* dot = strrchr(modPath, '.');
        if (dot) ext = dot;

        std::string outPath = dumpDir + '\\' + modName;
        if (!strrchr(modName, '.'))
            outPath += ext;

        if (PeDumper::DumpModule(base, outPath.c_str()))
            dumped++;
        else
            failed++;
    }

    dprintf("Module dump complete: %d dumped, %d failed, output: %s\n", dumped, failed, dumpDir.c_str());
}

// HOT PATH - every bridge call here costs about one to ten microseconds per instruction
void Tracer::cbTraceStep(CBTYPE, void* callbackInfo)
{
    auto info = (PLUG_CB_TRACEEXECUTE*)callbackInfo;
    Tracer& t = Tracer::Instance();

    if (!t.running.load())
        return;

    if (t.stopRequested.load())
    {
        info->stop = true;
        return;
    }

    if (info->stop)
    {
        t.needStepOver.store(false);
        t.traceDone.store(true);
        return;
    }

    duint cip = info->cip;

    auto modType = t.ClassifyModuleCached(cip);
    if (modType != Tracer::MOD_WHITELISTED)
    {
        if (t.skipMode == Tracer::SKIP_SILENT && t.logSkippedCalls && modType != Tracer::MOD_UNKNOWN)
        {
            BASIC_INSTRUCTION_INFO instrInfo;
            memset(&instrInfo, 0, sizeof(instrInfo));
            DbgDisasmFastAt(cip, &instrInfo);

            if (instrInfo.call)
            {
                duint target = DbgGetBranchDestination(cip);
                if (target != 0 && !(target >= t.moduleCache.base && target < t.moduleCache.end))
                {
                    char targetMod[MAX_MODULE_SIZE] = "";
                    Script::Module::NameFromAddr(target, targetMod);
                    if (targetMod[0] != '\0')
                    {
                        REGDUMP_AVX512 regdump;
                        DbgGetRegDumpEx(&regdump, sizeof(regdump));

                        char label[MAX_LABEL_SIZE] = "";
                        DbgGetLabelAt(target, SEG_DEFAULT, label);

                        Tracer::CallExtra extra;
                        extra.target = target;
                        strncpy(extra.targetMod, targetMod, sizeof(extra.targetMod) - 1);
                        extra.targetIsSystem = t.IsSystemModule(target);
                        if (label[0] != '\0')
                            snprintf(extra.func, sizeof(extra.func), "%s.%s", targetMod, label);
                        else
                            snprintf(extra.func, sizeof(extra.func), "%s.0x%llX", targetMod, (unsigned long long)target);

                        t.traceCounter.fetch_add(1);
                        t.LogInstruction(cip, instrInfo, regdump.regcontext, &extra);
                        t.FlushIfNeeded();
                    }
                }
            }
        }
        return;
    }

    if (t.moduleCache.base != t.lastTrackedBase)
    {
        std::lock_guard<std::mutex> lock(t.tracedModulesMutex);
        t.tracedModuleBases.insert(t.moduleCache.base);
        t.lastTrackedBase = t.moduleCache.base;
    }

    t.traceCounter.fetch_add(1);

    BASIC_INSTRUCTION_INFO instrInfo;
    memset(&instrInfo, 0, sizeof(instrInfo));
    DbgDisasmFastAt(cip, &instrInfo);

    Tracer::CallExtra callExtraStorage;
    Tracer::CallExtra* pendingExtra = nullptr;

    if (instrInfo.call)
    {
        duint target = DbgGetBranchDestination(cip);

        if (target != 0)
        {
            char targetMod[MAX_MODULE_SIZE] = "";
            bool isSystem = t.IsSystemModule(target);
            bool isSkippedUser = !isSystem && t.IsSkippedUserModule(target, targetMod);

            if (isSystem || isSkippedUser)
            {
                if (isSystem)
                    Script::Module::NameFromAddr(target, targetMod);

                char label[MAX_LABEL_SIZE] = "";
                DbgGetLabelAt(target, SEG_DEFAULT, label);

                REGDUMP_AVX512 regdump;
                DbgGetRegDumpEx(&regdump, sizeof(regdump));

                Tracer::CallExtra extra;
                extra.target = target;
                strncpy(extra.targetMod, targetMod, sizeof(extra.targetMod) - 1);
                extra.targetIsSystem = isSystem;
                if (label[0] != '\0')
                    snprintf(extra.func, sizeof(extra.func), "%s.%s", targetMod, label);
                else
                    snprintf(extra.func, sizeof(extra.func), "%s.0x%llX", targetMod, (unsigned long long)target);

                t.LogInstruction(cip, instrInfo, regdump.regcontext, &extra);
                t.FlushIfNeeded();

                if (t.skipMode == Tracer::SKIP_STEP_OVER && (isSystem ? t.skipSystemModules : true))
                {
                    strncpy(t.stepOverType, isSystem ? "syscall" : "usercall", sizeof(t.stepOverType));
                    strncpy(t.stepOverTarget, targetMod, sizeof(t.stepOverTarget) - 1);
                    t.stepOverTarget[sizeof(t.stepOverTarget) - 1] = '\0';
                    t.needStepOver.store(true);
                    t.traceDone.store(true);
                    info->stop = true;
                }
                return;
            }
            else
            {
                char label[MAX_LABEL_SIZE] = "";
                DbgGetLabelAt(target, SEG_DEFAULT, label);
                if (label[0] != '\0')
                {
                    Script::Module::NameFromAddr(target, targetMod);
                    callExtraStorage.target = target;
                    strncpy(callExtraStorage.targetMod, targetMod, sizeof(callExtraStorage.targetMod) - 1);
                    callExtraStorage.targetIsSystem = false;
                    snprintf(callExtraStorage.func, sizeof(callExtraStorage.func), "%s.%s", targetMod, label);
                    pendingExtra = &callExtraStorage;
                }
            }
        }
    }

    REGDUMP_AVX512 regdump;
    DbgGetRegDumpEx(&regdump, sizeof(regdump));

    t.LogInstruction(cip, instrInfo, regdump.regcontext, pendingExtra);
    t.FlushIfNeeded();
}

void Tracer::FlushIfNeeded()
{
    if (++flushCounter >= 1000)
    {
        flushCounter = 0;
        std::lock_guard<std::mutex> lock(fileMutex);
        if (outFile)
            fflush(outFile);
    }
}

void Tracer::TraceLoop()
{
    DebugLog("[TRACE] TraceLoop started");
    dbgStepOverCount.store(0);

    while (!stopRequested.load() && DbgIsDebugging())
    {
        traceDone.store(false);

        // Async DbgCmdExec instead of blocking DbgCmdExecDirect
        // the direct variant blocks the script thread, causing deadlock
        DbgCmdExec("ticnd 0");

        bool ticndStarted = false;
        bool ticndInterrupted = false;
        int waitMs = 0;
        while (!traceDone.load() && !stopRequested.load() && DbgIsDebugging())
        {
            Sleep(1);
            waitMs++;

            if (DbgIsRunning())
                ticndStarted = true;

            if (ticndStarted && !DbgIsRunning())
            {
                if (!stopRequested.load() && !traceDone.load())
                {
                    DebugLog("[TRACE] ticnd interrupted after %d ms (debugger paused - stopping trace)", waitMs);
                    ticndInterrupted = true;
                }
                break;
            }

            if (!ticndStarted && waitMs > 2000)
            {
                DebugLog("[TRACE] WARNING: ticnd failed to start after 2s");
                ticndInterrupted = true;
                break;
            }
        }

        if (stopRequested.load() || !DbgIsDebugging() || ticndInterrupted)
        {
            if (ticndInterrupted && !stopRequested.load() && DbgIsDebugging())
            {
                if (!finishAtFirstStop)
                {
                    DebugLog("[TRACE] Trace paused at breakpoint. %llu instructions so far.",
                        (unsigned long long)traceCounter.load());
                    {
                        std::lock_guard<std::mutex> lock(fileMutex);
                        if (outFile) fflush(outFile);
                    }
                    paused.store(true);
                    running.store(false);
                    return;
                }
                DebugLog("[TRACE] Trace interrupted at breakpoint, finishing session. %llu instructions.",
                    (unsigned long long)traceCounter.load());
                break;
            }

            DebugLog("[TRACE] TraceLoop breaking: stopRequested=%d, debugging=%d, interrupted=%d",
                (int)stopRequested.load(), (int)DbgIsDebugging(), (int)ticndInterrupted);
            break;
        }

        if (!needStepOver.load())
            continue;
        {
            uint64_t stoNum = dbgStepOverCount.fetch_add(1);

            duint stoCip = Script::Register::GetCIP();
            DebugLog("[TRACE] step-over #%llu: %s -> %s at 0x%llX",
                (unsigned long long)stoNum,
                stepOverType,
                stepOverTarget,
                (unsigned long long)stoCip);

            while (DbgIsRunning() && DbgIsDebugging())
                Sleep(1);

            stepDone.store(false);
            waitingForStep.store(true);

            DebugLog("[TRACE] issuing sto at 0x%llX", (unsigned long long)stoCip);
            DbgCmdExec("sto");

            int waitCycles = 0;
            bool stoStarted = false;
            while (!stepDone.load() && DbgIsDebugging() && !stopRequested.load())
            {
                Sleep(1);
                waitCycles++;
                if (DbgIsRunning()) stoStarted = true;

                if (stoStarted && !DbgIsRunning())
                {
                    Sleep(10);
                    if (!stepDone.load())
                    {
                        DebugLog("[TRACE] WARNING: sto paused without CB_STEPPED after %d ms (likely hit unrelated breakpoint)", waitCycles);
                        break;
                    }
                }
                if (!stoStarted && waitCycles > 2000)
                {
                    DebugLog("[TRACE] WARNING: sto failed to start after 2s");
                    break;
                }
            }

            if (!DbgIsDebugging())
                break;

            waitingForStep.store(false);
            needStepOver.store(false);
        }
    }

    {
        std::lock_guard<std::mutex> lock(fileMutex);
        if (outFile)
        {
            fflush(outFile);
            fclose(outFile);
            outFile = nullptr;
        }
    }

    traceEndTick.store(GetTickCount64());
    running.store(false);
    finished.store(true);

    DebugLog("[TRACE] Trace finished. %llu instructions, %llu step-overs.",
        (unsigned long long)traceCounter.load(),
        (unsigned long long)dbgStepOverCount.load());

    CloseDebugLog();

    if (dumpTracedModules)
    {
        dputs("Dumping traced user modules...");
        DumpTracedModules();
    }
}

void Tracer::LogInstruction(duint cip, const BASIC_INSTRUCTION_INFO& instrInfo, const REGISTERCONTEXT_AVX512& regs, const CallExtra* callExtra)
{
    const char* modName = moduleCache.name;

    unsigned char rawBytes[16];
    memset(rawBytes, 0, sizeof(rawBytes));
    int instrSize = instrInfo.size;
    if (instrSize > 16) instrSize = 16;
    if (instrSize > 0)
        DbgMemRead(cip, rawBytes, instrSize);

    char hexBytes[48];
    for (int i = 0; i < instrSize; i++)
    {
        static const char hex[] = "0123456789ABCDEF";
        hexBytes[i * 2]     = hex[rawBytes[i] >> 4];
        hexBytes[i * 2 + 1] = hex[rawBytes[i] & 0xF];
    }
    hexBytes[instrSize * 2] = '\0';

    regsBuf.clear();
    BuildRegsJson(regs, regsBuf);

    lineBuf.clear();
    char buf[256];

    snprintf(buf, sizeof(buf),
        "{\"c\":%llu,\"mod\":\"%s\",\"ip\":\"0x%llX\",\"raw\":\"%s\",\"dis\":\"%s\"",
        (unsigned long long)traceCounter.load(),
        modName,
        (unsigned long long)cip,
        hexBytes,
        JsonEscape(instrInfo.instruction).c_str());
    lineBuf += buf;

    lineBuf += ",\"regs\":{";
    lineBuf += regsBuf;
    lineBuf += "}";

    snprintf(buf, sizeof(buf), ",\"flags\":\"0x%llX\"", (unsigned long long)regs.eflags);
    lineBuf += buf;

    if (callExtra)
    {
        char extrabuf[640];
        snprintf(extrabuf, sizeof(extrabuf),
            ",\"extra\":{\"target\":\"0x%llX\",\"target_mod\":\"%s\",\"target_type\":\"%s\",\"func\":\"%s\"}",
            (unsigned long long)callExtra->target,
            JsonEscape(callExtra->targetMod).c_str(),
            callExtra->targetIsSystem ? "system" : "user",
            JsonEscape(callExtra->func).c_str());
        lineBuf += extrabuf;
    }

    lineBuf += "}\n";

    WriteLine(lineBuf.c_str(), lineBuf.size());
}

void Tracer::BuildRegsJson(const REGISTERCONTEXT_AVX512& regs, std::string& out)
{
    struct RegEntry { const char* name; duint value; };

    RegEntry entries[] = {
        {"rax", (duint)regs.cax}, {"rbx", (duint)regs.cbx},
        {"rcx", (duint)regs.ccx}, {"rdx", (duint)regs.cdx},
        {"rsi", (duint)regs.csi}, {"rdi", (duint)regs.cdi},
        {"rbp", (duint)regs.cbp}, {"rsp", (duint)regs.csp},
        {"r8",  (duint)regs.r8},  {"r9",  (duint)regs.r9},
        {"r10", (duint)regs.r10}, {"r11", (duint)regs.r11},
        {"r12", (duint)regs.r12}, {"r13", (duint)regs.r13},
        {"r14", (duint)regs.r14}, {"r15", (duint)regs.r15},
    };

    bool first = true;
    char buf[96];
    for (const auto& r : entries)
    {
        if (r.value == 0)
            continue;

        if (!first)
            out += ",";
        first = false;

        std::string resolved = FormatRegValue(r.value);
        if (resolved.empty())
        {
            snprintf(buf, sizeof(buf), "\"%s\":\"0x%llX\"", r.name, (unsigned long long)r.value);
            out += buf;
        }
        else
        {
            snprintf(buf, sizeof(buf), "\"%s\":{\"value\":\"0x%llX\",\"string\":\"", r.name, (unsigned long long)r.value);
            out += buf;
            out += JsonEscape(resolved.c_str());
            out += "\"}";
        }
    }

    char xmmBuf[128];
    for (int i = 0; i < 16; i++)
    {
        const XMMREGISTER& xmm = reinterpret_cast<const XMMREGISTER&>(regs.ZmmRegisters[i]);
        if (xmm.Low == 0 && xmm.High == 0)
            continue;

        if (!first)
            out += ",";
        first = false;

        snprintf(xmmBuf, sizeof(xmmBuf), "\"xmm%d\":\"0x%016llX%016llX\"",
            i, (unsigned long long)xmm.High, (unsigned long long)xmm.Low);
        out += xmmBuf;
    }
}


void Tracer::WriteLine(const char* line, size_t len)
{
    std::lock_guard<std::mutex> lock(fileMutex);
    if (outFile)
        fwrite(line, 1, len, outFile);
}

Tracer::ModuleType Tracer::ClassifyModuleCached(duint addr)
{
    if (addr >= moduleCache.base && addr < moduleCache.end && moduleCache.base != 0)
    {
        if (moduleCache.isSystem) return MOD_SYSTEM;
        return moduleCache.isWhitelisted ? MOD_WHITELISTED : MOD_SKIPPED;
    }

    duint base = Script::Module::BaseFromAddr(addr);
    if (base == 0)
    {
        moduleCache.base = 0;
        moduleCache.name[0] = '\0';
        return MOD_UNKNOWN;
    }

    duint size = Script::Module::SizeFromAddr(base);
    moduleCache.base = base;
    moduleCache.end = base + size;

    if (!Script::Module::NameFromAddr(addr, moduleCache.name))
        moduleCache.name[0] = '\0';

    if (skipSystemModules && DbgFunctions()->ModGetParty(base) == mod_system)
    {
        moduleCache.isSystem = true;
        moduleCache.isWhitelisted = false;
        return MOD_SYSTEM;
    }

    moduleCache.isSystem = false;

    if (skipUserModules.count(ToLower(moduleCache.name)))
    {
        moduleCache.isWhitelisted = false;
        return MOD_SKIPPED;
    }

    moduleCache.isWhitelisted = true;
    return MOD_WHITELISTED;
}

bool Tracer::IsSystemModule(duint addr)
{
    duint base = Script::Module::BaseFromAddr(addr);
    return base != 0 && DbgFunctions()->ModGetParty(base) == mod_system;
}

bool Tracer::IsSkippedUserModule(duint addr, char* modNameOut)
{
    char modName[MAX_MODULE_SIZE] = "";
    if (!Script::Module::NameFromAddr(addr, modName))
        return false;

    if (skipUserModules.count(ToLower(modName)))
    {
        if (modNameOut)
            strcpy(modNameOut, modName);
        return true;
    }
    return false;
}

bool Tracer::IsWhitelistedModule(duint addr)
{
    duint base = Script::Module::BaseFromAddr(addr);
    if (base == 0)
        return false;

    if (skipSystemModules && DbgFunctions()->ModGetParty(base) == mod_system)
        return false;

    char modName[MAX_MODULE_SIZE] = "";
    if (!Script::Module::NameFromAddr(addr, modName))
        return false;

    return !skipUserModules.count(ToLower(modName));
}

std::string Tracer::ReadStringAt(duint addr, int maxLen)
{
    if (addr < 0x10000)
        return "";
    if (addr > 0x00007FFFFFFFFFFF)
        return "";

    if (!DbgMemIsValidReadPtr(addr))
        return "";

    char buf[260];
    int readLen = (maxLen < (int)sizeof(buf)) ? maxLen : (int)sizeof(buf) - 1;
    memset(buf, 0, sizeof(buf));

    if (!DbgMemRead(addr, buf, readLen))
        return "";

    int printable = 0;
    for (int i = 0; i < readLen; i++)
    {
        auto c = (unsigned char)buf[i];
        if (c == 0)
            break;
        if (c >= 0x20 && c < 0x7F)
            printable++;
        else
        {
            printable = 0;
            break;
        }
    }

    if (printable >= 4)
    {
        buf[readLen] = 0;
        return std::string(buf);
    }

    wchar_t wbuf[130];
    readLen = (maxLen < (int)(sizeof(wbuf) - sizeof(wchar_t))) ? maxLen : (int)(sizeof(wbuf) - sizeof(wchar_t));
    memset(wbuf, 0, sizeof(wbuf));

    if (!DbgMemRead(addr, wbuf, readLen) || readLen < 4)
        return "";

    int wPrintable = 0;
    int wLen = readLen / (int)sizeof(wchar_t);
    for (int i = 0; i < wLen; i++)
    {
        wchar_t c = wbuf[i];
        if (c == 0)
            break;
        if (c >= 0x20 && c < 0x7F)
            wPrintable++;
        else
        {
            wPrintable = 0;
            break;
        }
    }

    if (wPrintable >= 4)
    {
        std::string narrow;
        for (int i = 0; i < wLen && wbuf[i] != 0; i++)
            narrow += (char)wbuf[i];
        return narrow;
    }

    return "";
}

std::string Tracer::FormatRegValue(duint val)
{
    return val ? ReadStringAt(val, 128) : "";
}

std::string Tracer::JsonEscape(const char* s)
{
    std::string out;
    out.reserve(strlen(s) + 16);
    for (const char* p = s; *p; p++)
    {
        switch (*p)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)*p < 0x20)
            {
                char hex[8];
                snprintf(hex, sizeof(hex), "\\u%04X", (unsigned char)*p);
                out += hex;
            }
            else
                out += *p;
            break;
        }
    }
    return out;
}
