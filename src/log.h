#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace dsvr
{
    inline FILE*& LogFile()
    {
        static FILE* file = nullptr;
        return file;
    }

    // Opens dsvr.log next to our DLL (i.e. in the game directory).
    inline void OpenLog(HMODULE module)
    {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
        while (len > 0 && path[len - 1] != '\\')
            --len;
        path[len] = '\0';
        strcat_s(path, "dsvr.log");
        fopen_s(&LogFile(), path, "w");
    }

    inline void CloseLog()
    {
        if (LogFile())
        {
            fclose(LogFile());
            LogFile() = nullptr;
        }
    }

    inline void Log(const char* fmt, ...)
    {
        FILE* file = LogFile();
        if (!file)
            return;

        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

        va_list args;
        va_start(args, fmt);
        vfprintf(file, fmt, args);
        va_end(args);

        fputc('\n', file);
        fflush(file);
    }
}
