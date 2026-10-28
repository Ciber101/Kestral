// Kestral CAD: personal parametric CAD. Entry point.
#include "app/App.h"
#include "kernel/Kernel.h"
#include "print/BambuCloud.h"
#include "print/Orca.h"
#include "platform/Platform.h"

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
    if (argc >= 3 && std::string(argv[1]) == "--slice-test")
        return kst::print::sliceSelfTest(argv[2], argc >= 4 ? argv[3] : "");
    kst::App app;
    return app.run(argc, argv);
}
