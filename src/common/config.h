#pragma once

#include <windows.h>
#include <cstdlib>

namespace dsvr
{
    // Settings live in dsvr.ini next to our DLLs (the game folder). Missing keys use the defaults.
    class Config
    {
    public:
        explicit Config(HMODULE module)
        {
            DWORD len = GetModuleFileNameA(module, m_path, MAX_PATH);
            while (len > 0 && m_path[len - 1] != '\\')
                --len;
            m_path[len] = '\0';
            strcat_s(m_path, "dsvr.ini");
        }

        const char* Path() const { return m_path; }

        float Float(const char* section, const char* key, float def) const
        {
            char buf[64];
            if (!GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), m_path) || !buf[0])
                return def;
            return static_cast<float>(atof(buf));
        }

        // Accepts decimal or 0x-prefixed hex (handy for virtual-key codes).
        int Int(const char* section, const char* key, int def) const
        {
            char buf[64];
            if (!GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), m_path) || !buf[0])
                return def;
            return static_cast<int>(strtol(buf, nullptr, 0));
        }

        bool Bool(const char* section, const char* key, bool def) const
        {
            return Int(section, key, def ? 1 : 0) != 0;
        }

    private:
        char m_path[MAX_PATH];
    };
}
