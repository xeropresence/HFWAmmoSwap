#include "crashlog.h"

#include "log.h"

#include <Windows.h>
#include <Psapi.h>

#include <cstdio>

namespace
{
    LONG g_reported;

    void Describe(uintptr_t address, char* out, size_t size)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &module))
        {
            char name[MAX_PATH] = {};
            GetModuleBaseNameA(GetCurrentProcess(), module, name, sizeof(name));
            sprintf_s(out, size, "%s+0x%llX", name, static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
        }
        else
        {
            sprintf_s(out, size, "0x%llX", static_cast<unsigned long long>(address));
        }
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
            return EXCEPTION_CONTINUE_SEARCH;
        if (InterlockedIncrement(&g_reported) > 8)
            return EXCEPTION_CONTINUE_SEARCH;

        CONTEXT ctx = *info->ContextRecord;
        char where[128];
        Describe(ctx.Rip, where, sizeof(where));
        LOG("EXCEPTION 0x%08X at %s (access %llu @ 0x%llX)", code, where,
            static_cast<unsigned long long>(info->ExceptionRecord->NumberParameters > 0 ? info->ExceptionRecord->ExceptionInformation[0] : 0),
            static_cast<unsigned long long>(info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0));
        LOG("  rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX r8=%llX r9=%llX", ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.R8, ctx.R9);

        for (int frame = 0; frame < 24 && ctx.Rip; frame++)
        {
            Describe(ctx.Rip, where, sizeof(where));
            LOG("  #%02d %s", frame, where);

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn)
            {
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace crashlog
{
    void Install() { AddVectoredExceptionHandler(1, Handler); }
}
