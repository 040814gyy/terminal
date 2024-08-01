// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CONPTY_IMPEXP

#include <Windows.h>

#include <conpty-static.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>

#define CONSOLE_READ_NOWAIT 0x0002

BOOL WINAPI ReadConsoleInputExA(
    _In_ HANDLE hConsoleInput,
    _Out_writes_(nLength) PINPUT_RECORD lpBuffer,
    _In_ DWORD nLength,
    _Out_ LPDWORD lpNumberOfEventsRead,
    _In_ USHORT wFlags);

// Forward declare the bits from types/inc/utils.hpp that we need so we don't need to pull in a dozen STL headers.
namespace Microsoft::Console::Utils
{
    struct Pipe
    {
        wil::unique_hfile server;
        wil::unique_hfile client;
    };
    Pipe CreateOverlappedPipe(DWORD openMode, DWORD bufferSize);
}

static Microsoft::Console::Utils::Pipe pipe;

static COORD getViewportSize()
{
    CONSOLE_SCREEN_BUFFER_INFOEX csbiex{ .cbSize = sizeof(csbiex) };
    THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfoEx(GetStdHandle(STD_OUTPUT_HANDLE), &csbiex));
    const SHORT w = csbiex.srWindow.Right - csbiex.srWindow.Left + 1;
    const SHORT h = csbiex.srWindow.Bottom - csbiex.srWindow.Top + 1;
    return { w, h };
}

static void run()
{
    static const auto pReadConsoleInputExA = GetProcAddressByFunctionDeclaration(GetModuleHandleW(L"kernel32.dll"), ReadConsoleInputExA);
    THROW_LAST_ERROR_IF_NULL(pReadConsoleInputExA);

    pipe = Microsoft::Console::Utils::CreateOverlappedPipe(PIPE_ACCESS_DUPLEX, 128 * 1024);

    auto viewportSize = getViewportSize();

    HPCON hPC = nullptr;
    THROW_IF_FAILED(ConptyCreatePseudoConsole(viewportSize, pipe.client.get(), pipe.client.get(), 0, &hPC));

    PROCESS_INFORMATION pi;
    {
        wchar_t commandLine[MAX_PATH] = LR"(C:\Windows\System32\cmd.exe)";

        STARTUPINFOEX siEx{};
        siEx.StartupInfo.cb = sizeof(STARTUPINFOEX);
        siEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

        char attrList[128];
        size_t size = sizeof(attrList);
        siEx.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(&attrList[0]);
        THROW_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &size));
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(siEx.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(HPCON), nullptr, nullptr));

        THROW_IF_WIN32_BOOL_FALSE(CreateProcessW(
            nullptr, // lpApplicationName
            commandLine, // lpCommandLine
            nullptr, // lpProcessAttributes
            nullptr, // lpThreadAttributes
            false, // bInheritHandles
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, // dwCreationFlags
            nullptr, // lpEnvironment
            nullptr, // lpCurrentDirectory
            &siEx.StartupInfo, // lpStartupInfo
            &pi // lpProcessInformation
            ));
    }

    THROW_IF_FAILED(ConptyReleasePseudoConsole(hPC));

    const auto inputHandle = GetStdHandle(STD_INPUT_HANDLE);
    const auto outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    OVERLAPPED outputConptyOverlapped{ .hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    HANDLE handles[] = { inputHandle, outputConptyOverlapped.hEvent };
    INPUT_RECORD records[4096];
    char inputConptyBuffer[ARRAYSIZE(records)];
    char outputConptyBuffer[256 * 1024];

    SetConsoleCtrlHandler(
        [](DWORD type) -> BOOL {
            switch (type)
            {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
                WriteFile(pipe.server.get(), "\x03", 1, nullptr, nullptr);
                return true;
            default:
                return false;
            }
        },
        TRUE);

    //SYSTEMTIME systemTime;
    //GetSystemTime(&systemTime);
    //char path[MAX_PATH];
    //sprintf_s(path, "%04d-%02d-%02dT%02d-%02d-%02dZ.txt", systemTime.wYear, systemTime.wMonth, systemTime.wDay, systemTime.wHour, systemTime.wMinute, systemTime.wSecond);
    //const auto dump = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    for (DWORD read; ReadFile(pipe.server.get(), &outputConptyBuffer[0], sizeof(outputConptyBuffer), &read, &outputConptyOverlapped);)
    {
        DWORD written;
        //WriteFile(dump, &outputConptyBuffer[0], read, &written, nullptr);
        if (!WriteFile(outputHandle, &outputConptyBuffer[0], read, &written, nullptr) || written != read)
        {
            return;
        }
    }
    if (GetLastError() != ERROR_IO_PENDING)
    {
        return;
    }

    for (;;)
    {
        switch (WaitForMultipleObjectsEx(ARRAYSIZE(handles), &handles[0], FALSE, INFINITE, FALSE))
        {
        case WAIT_OBJECT_0 + 0:
        {
            DWORD read;
            if (!pReadConsoleInputExA(inputHandle, &records[0], ARRAYSIZE(records), &read, CONSOLE_READ_NOWAIT) || read == 0)
            {
                return;
            }

            DWORD write = 0;
            for (DWORD i = 0; i < read; ++i)
            {
                switch (records[i].EventType)
                {
                case KEY_EVENT:
                    if (records[i].Event.KeyEvent.bKeyDown)
                    {
                        inputConptyBuffer[write++] = records[i].Event.KeyEvent.uChar.AsciiChar;
                    }
                    break;
                case WINDOW_BUFFER_SIZE_EVENT:
                    if (const auto size = getViewportSize(); memcmp(&viewportSize, &size, sizeof(COORD)) != 0)
                    {
                        viewportSize = size;
                        THROW_IF_FAILED(ConptyResizePseudoConsole(hPC, records[i].Event.WindowBufferSizeEvent.dwSize));
                    }
                    break;
                default:
                    break;
                }
            }

            if (write != 0)
            {
                DWORD written;
                if (!WriteFile(pipe.server.get(), &inputConptyBuffer[0], write, &written, nullptr) || written != read)
                {
                    return;
                }
            }
            break;
        }
        case WAIT_OBJECT_0 + 1:
        {
            DWORD read2;
            if (!GetOverlappedResult(pipe.server.get(), &outputConptyOverlapped, &read2, FALSE))
            {
                return;
            }

            do
            {
                DWORD written;
                //WriteFile(dump, &outputConptyBuffer[0], read, &written, nullptr);
                if (!WriteFile(outputHandle, &outputConptyBuffer[0], read2, &written, nullptr) || written != read2)
                {
                    return;
                }
            } while (ReadFile(pipe.server.get(), &outputConptyBuffer[0], sizeof(outputConptyBuffer), &read2, &outputConptyOverlapped));

            if (GetLastError() != ERROR_IO_PENDING)
            {
                return;
            }
            break;
        }
        default:
            return;
        }
    }
}

int wmain(int argc, WCHAR* argv[])
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    const auto inputHandle = GetStdHandle(STD_INPUT_HANDLE);
    const auto outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD previousInputMode = 0;
    GetConsoleMode(inputHandle, &previousInputMode);
    SetConsoleMode(inputHandle, ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS | ENABLE_VIRTUAL_TERMINAL_INPUT);

    DWORD previousOutputMode = 0;
    GetConsoleMode(outputHandle, &previousOutputMode);
    SetConsoleMode(outputHandle, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);

    const auto previousInputCP = GetConsoleCP();
    const auto previousOutputCP = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    int exitCode = 0;

    try
    {
        run();
    }
    catch (const wil::ResultException& e)
    {
        printf("Error: %s\n", e.what());
        exitCode = e.GetErrorCode();
    }

    SetConsoleMode(outputHandle, previousOutputMode);
    SetConsoleCP(previousInputCP);
    SetConsoleOutputCP(previousOutputCP);

    return exitCode;
}
