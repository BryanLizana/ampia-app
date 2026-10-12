#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "Crash.h"

#if JUCE_WINDOWS
 #define NOMINMAX
 #include <windows.h>
 #include <dbghelp.h>
 #pragma comment (lib, "dbghelp.lib")
#endif

// Si la app se cae (casi siempre dentro de un plugin, como AmpliTube), deja en
// ampbox.log en qué hilo, en qué módulo y la pila de llamadas, y un .dmp en
// %APPDATA%\AmpBox para abrirlo con un depurador (Visual Studio / WinDbg).
namespace
{
    juce::File dataFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("AmpBox");
    }

    void onCrash (void* info)
    {
        juce::String text;
        text << "\n===== CRASH " << juce::Time::getCurrentTime().toString (true, true) << " =====\n";
        text << "hilo: " << (juce::MessageManager::existsAndIsCurrentThread() ? juce::String ("ventana (mensajes)")
                                                                              : juce::String ("otro (audio, MIDI o de fondo)"))
             << "\n";

       #if JUCE_WINDOWS
        auto* pointers = static_cast<EXCEPTION_POINTERS*> (info);
        if (pointers != nullptr && pointers->ExceptionRecord != nullptr)
        {
            auto* address = pointers->ExceptionRecord->ExceptionAddress;
            text << "codigo: 0x" << juce::String::toHexString ((juce::int64) pointers->ExceptionRecord->ExceptionCode)
                 << "  direccion: 0x" << juce::String::toHexString ((juce::pointer_sized_int) address) << "\n";

            HMODULE module = nullptr;
            wchar_t path[MAX_PATH] = {};
            if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCWSTR) address, &module)
                && GetModuleFileNameW (module, path, MAX_PATH) > 0)
                text << "modulo: " << juce::String (path) << "  (+0x"
                     << juce::String::toHexString ((juce::pointer_sized_int) address - (juce::pointer_sized_int) module) << ")\n";
        }

        const auto dump = dataFolder().getChildFile ("crash-" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H%M%S") + ".dmp");
        HANDLE file = CreateFileW (dump.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION exception { GetCurrentThreadId(), pointers, FALSE };
            const bool ok = MiniDumpWriteDump (GetCurrentProcess(), GetCurrentProcessId(), file,
                                               (MINIDUMP_TYPE) (MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                                               pointers != nullptr ? &exception : nullptr, nullptr, nullptr);
            CloseHandle (file);
            text << "volcado: " << (ok ? dump.getFileName() : juce::String ("no se pudo escribir")) << "\n";
        }
       #else
        juce::ignoreUnused (info);
       #endif

        text << "pila:\n" << juce::SystemStats::getStackBacktrace() << "\n";
        dataFolder().getChildFile ("ampbox.log").appendText (text);
    }
}

void installCrashHandler()
{
    juce::SystemStats::setApplicationCrashHandler (onCrash);
}
