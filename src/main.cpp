// Kestral CAD: personal parametric CAD. Entry point.
#include "app/App.h"
#include "app/Mcp.h"
#include "kernel/Kernel.h"
#include "print/BambuCloud.h"
#include "print/Orca.h"
#include "platform/Platform.h"

#include <algorithm>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

int main(int argc, char** argv) {
    // Helper mode: Kestral started by itself to read one big STEP file (no window).
    if (argc >= 2 && std::string(argv[1]) == "--import-worker") {
        std::vector<std::string> a;
#ifdef _WIN32
        // argv is in the ANSI code page; paths need the UTF-8 form.
        int n = 0;
        if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
            for (int i = 0; i < n; ++i) {
                int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
                std::string s(len > 0 ? (size_t)len - 1 : 0, '\0');
                if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
                a.push_back(s);
            }
            LocalFree(w);
        }
#else
        for (int i = 0; i < argc; ++i) a.push_back(argv[i]);
#endif
        if (a.size() < 5) return 64;
        return kst::kernel::runImportWorker(a[2], a[3], a[4]);
    }
    // Helper mode: Bambu's network plugin does one job (status or print) and Kestral's helper ends.
    if (argc >= 2 && std::string(argv[1]) == "--bambu-agent") {
        std::vector<std::string> a;
#ifdef _WIN32
        int n = 0;
        if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
            for (int i = 0; i < n; ++i) {
                int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
                std::string s(len > 0 ? (size_t)len - 1 : 0, '\0');
                if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
                a.push_back(s);
            }
            LocalFree(w);
        }
#else
        for (int i = 0; i < argc; ++i) a.push_back(argv[i]);
#endif
        std::vector<char*> av;
        for (auto& s : a) av.push_back(s.data());
        int rc = kst::print::runBambuAgent((int)av.size(), av.data());
        kst::platform::hardExit(rc);   // the plugin's own threads must not hold the helper open
    }
    // MCP stdio bridge: an AI app started Kestral to talk to the open Kestral window (see app/Mcp.h).
    if (argc >= 2 && std::string(argv[1]) == "--mcp") {
        bool ro = false;
        for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--read-only") ro = true;
        return kst::mcp::runBridge(ro);
    }
    if (argc >= 3 && std::string(argv[1]) == "--slice-test")
        return kst::print::sliceSelfTest(argv[2], argc >= 4 ? argv[3] : "");
#ifdef _WIN32
    // A runaway step (a boolean that blows up, a mesh that never ends) must not take the whole PC
    // down with it: past 60% of the RAM, Kestral's own allocations fail (the step reports an error,
    // or Kestral closes and the watchdog's recovery copy brings the work back) instead of Windows
    // running out of memory for everything else.
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0) {
            unsigned long long cap = std::max<unsigned long long>(ms.ullTotalPhys * 6 / 10, 4ull << 30);
            if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
                li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
                li.ProcessMemoryLimit = (SIZE_T)cap;
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
                    !AssignProcessToJobObject(job, GetCurrentProcess()))
                    CloseHandle(job);   // not allowed here: run without the cap
            }
        }
    }
#endif
    kst::App app;
    return app.run(argc, argv);
}
