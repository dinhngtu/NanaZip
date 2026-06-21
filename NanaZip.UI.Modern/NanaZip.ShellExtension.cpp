/*
 * PROJECT:    NanaZip
 * FILE:       NanaZip.ShellExtension.cpp
 * PURPOSE:    Implementation for NanaZip Shell Extension
 *
 * LICENSE:    The MIT License
 *
 * MAINTAINER: MouriNaruto (Kenji.Mouri@outlook.com)
 */

#include <Windows.h>

#include <combaseapi.h>
#include <KnownFolders.h>
#include <atomic>
#include <cstdarg>
#include <exception>
#include <ocidl.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

#include <shlobj_core.h>
#include <shobjidl_core.h>

#include <strsafe.h>

#include <winrt/Windows.Foundation.h>

#include "../SevenZip/CPP/Common/Common.h"
#include "../SevenZip/CPP/Windows/DLL.h"
#include "../SevenZip/CPP/Windows/FileDir.h"
#include "../SevenZip/CPP/Windows/FileFind.h"
#include "../SevenZip/CPP/Windows/FileName.h"
#include "../SevenZip/CPP/Windows/ProcessUtils.h"
#include "../SevenZip/CPP/7zip/UI/Common/ArchiveName.h"
#include "../SevenZip/CPP/7zip/UI/Common/CompressCall.h"
#include "../SevenZip/CPP/7zip/UI/Common/ExtractingFilePath.h"
#include "../SevenZip/CPP/7zip/UI/Common/ZipRegistry.h"
#include "../SevenZip/CPP/7zip/UI/FileManager/FormatUtils.h"
#include "../SevenZip/CPP/7zip/UI/FileManager/LangUtils.h"
#include "../SevenZip/CPP/7zip/UI/Explorer/ContextMenuFlags.h"
#include "../SevenZip/CPP/7zip/UI/Explorer/resource.h"

namespace
{
    static const char* const kExtractExcludeExtensions =
        " 3gp"
        " aac ans ape asc asm asp aspx avi awk"
        " bas bat bmp"
        " c cs cls clw cmd cpp csproj css ctl cxx"
        " def dep dlg dsp dsw"
        " eps"
        " f f77 f90 f95 fla flac frm"
        " gif"
        " h hpp hta htm html hxx"
        " ico idl inc ini inl"
        " java jpeg jpg js"
        " la lnk log"
        " mak manifest wmv mov mp3 mp4 mpe mpeg mpg m4a"
        " ofr ogg"
        " pac pas pdf php php3 php4 php5 phptml pl pm png ps py pyo"
        " ra rb rc reg rka rm rtf"
        " sed sh shn shtml sln sql srt swa"
        " tcl tex tiff tta txt"
        " vb vcproj vbs"
        " wav webp wma wv"
        " xml xsd xsl xslt"
        " ";

    static bool FindExt(const char* p, const FString& name)
    {
        int dotPos = name.ReverseFind_Dot();
        if (dotPos < 0 || dotPos == (int)name.Len() - 1)
            return false;

        AString s;

        for (unsigned pos = dotPos + 1;; pos++)
        {
            wchar_t c = name[pos];
            if (c == 0)
                break;
            if (c >= 0x80)
                return false;
            s += (char)MyCharLower_Ascii((char)c);
        }

        for (unsigned i = 0; p[i] != 0;)
        {
            unsigned j;
            for (j = i; p[j] != ' '; j++);
            if (s.Len() == j - i && memcmp(p + i, (const char*)s, s.Len()) == 0)
                return true;
            i = j + 1;
        }

        return false;
    }

    static bool DoNeedExtract(const FString& name)
    {
        return !FindExt(kExtractExcludeExtensions, name);
    }

    static const char* const kArcExts[] =
    {
        "7z"
      , "bz2"
      , "gz"
      , "lz"
      , "liz"
      , "lz4"
      , "lz5"
      , "rar"
      , "zip"
      , "zst"
    };

    static bool IsItArcExt(const UString& ext)
    {
        for (unsigned i = 0; i < ARRAY_SIZE(kArcExts); i++)
            if (ext.IsEqualTo_Ascii_NoCase(kArcExts[i]))
                return true;
        return false;
    }

    UString GetSubFolderNameForExtract(const UString& arcName)
    {
        int dotPos = arcName.ReverseFind_Dot();
        if (dotPos < 0)
            return Get_Correct_FsFile_Name(arcName) + L'~';

        const UString ext = arcName.Ptr(dotPos + 1);
        UString res = arcName.Left(dotPos);
        res.TrimRight();
        dotPos = res.ReverseFind_Dot();
        if (dotPos > 0)
        {
            const UString ext2 = res.Ptr(dotPos + 1);
            if ((ext.IsEqualTo_Ascii_NoCase("001") && IsItArcExt(ext2))
                || (ext.IsEqualTo_Ascii_NoCase("rar") &&
                    (ext2.IsEqualTo_Ascii_NoCase("part001")
                        || ext2.IsEqualTo_Ascii_NoCase("part01")
                        || ext2.IsEqualTo_Ascii_NoCase("part1"))))
                res.DeleteFrom(dotPos);
            res.TrimRight();
        }
        return Get_Correct_FsFile_Name(res);
    }

    static void ReduceString(UString& s)
    {
        const unsigned kMaxSize = 60;
        if (s.Len() <= kMaxSize)
            return;
        s.Delete(kMaxSize / 2, s.Len() - kMaxSize);
        s.Insert(kMaxSize / 2, L" ... ");
    }

    // Backport GetQuotedReducedString from 7-Zip ZS since GetQuotedString
    // now does string escaping as well.
    static UString GetQuotedReducedString(const UString& s)
    {
        UString s2 = s;
        ReduceString(s2);
        s2.Replace(L"&", L"&&");
        s2.InsertAtFront(L'"');
        s2 += L'"'; // quote without GetQuotedString (because it escapes now)
        return s2;
    }

    static void MyFormatNew_ReducedName(UString& s, const UString& name)
    {
        s = MyFormatNew(s, GetQuotedReducedString(name));
    }

    static UString GetNanaZipPath()
    {
        return fs2us(NWindows::NDLL::GetModuleDirPrefix()) + L"NanaZip.Modern.FileManager.exe";
    }

    static constexpr DWORD kLongRunningReportDelayMilliseconds = 10 * 1000;
    static constexpr DWORD kLongRunningReportPeriodMilliseconds = 10 * 1000;

    static std::atomic<unsigned long long> g_ProcessStartTick = 0;
    static std::atomic<void*> g_LongRunningReportTimer = nullptr;
    static std::atomic<bool> g_LongRunningReportTimerStarted = false;
    static std::atomic<long> g_LogFilePathState = 0;
    static wchar_t g_LogFilePath[MAX_PATH] = {};
    static constexpr unsigned long kComProcessReferenceProbeUnavailable =
        static_cast<unsigned long>(-1);
    static std::atomic<unsigned long> g_ProcessAttachComProcessRefAfterAdd =
        kComProcessReferenceProbeUnavailable;
    static std::atomic<unsigned long> g_ProcessAttachComProcessRefAfterRelease =
        kComProcessReferenceProbeUnavailable;
    static std::atomic<bool> g_ProcessAttachComProcessRefLogged = false;

    struct ShellExtensionObjectCounters
    {
        std::atomic<long long> Created = 0;
        std::atomic<long long> Destroyed = 0;
    };

    static ShellExtensionObjectCounters g_ExplorerCommandBaseCounters;
    static ShellExtensionObjectCounters g_ExplorerCommandRootCounters;
    static ShellExtensionObjectCounters g_ClassFactoryCounters;

    static ULONGLONG GetProcessStartTick()
    {
        ULONGLONG StartTick = g_ProcessStartTick.load();
        if (StartTick == 0)
        {
            ULONGLONG CurrentTick = ::GetTickCount64();
            if (g_ProcessStartTick.compare_exchange_strong(
                StartTick,
                CurrentTick))
            {
                StartTick = CurrentTick;
            }
        }
        return StartTick;
    }

    static ULONGLONG GetProcessAgeMilliseconds()
    {
        return ::GetTickCount64() - GetProcessStartTick();
    }

    static long long GetCurrentModuleLockCount()
    {
        return static_cast<long long>(winrt::get_module_lock());
    }

    struct ComServerProcessReferenceProbe
    {
        unsigned long AfterAdd = 0;
        unsigned long AfterRelease = 0;
    };

    static ComServerProcessReferenceProbe ProbeComServerProcessReferenceForLog()
    {
        ComServerProcessReferenceProbe Probe;
        Probe.AfterAdd = ::CoAddRefServerProcess();
        Probe.AfterRelease = ::CoReleaseServerProcess();
        return Probe;
    }

    static bool AppendPathComponent(
        wchar_t* Path,
        size_t PathCapacity,
        const wchar_t* Component)
    {
        UNREFERENCED_PARAMETER(PathCapacity);
        return !!::PathAppendW(Path, Component);
    }

    static bool CreateDirectoryIfNeeded(const wchar_t* Path)
    {
        if (::CreateDirectoryW(Path, nullptr))
        {
            return true;
        }

        return (::GetLastError() == ERROR_ALREADY_EXISTS);
    }

    static bool TryBuildLogFilePath(
        wchar_t* Path,
        size_t PathCapacity)
    {
        if (!Path || PathCapacity == 0)
        {
            return false;
        }

        Path[0] = L'\0';

        PWSTR LocalAppData = nullptr;
        HRESULT Result = ::SHGetKnownFolderPath(
            FOLDERID_LocalAppData,
            KF_FLAG_DEFAULT,
            nullptr,
            &LocalAppData);
        if (SUCCEEDED(Result) && LocalAppData)
        {
            Result = ::StringCchCopyW(Path, PathCapacity, LocalAppData);
            ::CoTaskMemFree(LocalAppData);
            if (FAILED(Result))
            {
                return false;
            }
        }
        else
        {
            DWORD Length = ::GetEnvironmentVariableW(
                L"LOCALAPPDATA",
                Path,
                static_cast<DWORD>(PathCapacity));
            if (Length == 0 || Length >= PathCapacity)
            {
                return false;
            }
        }

        if (!AppendPathComponent(Path, PathCapacity, L"NanaZip") ||
            !CreateDirectoryIfNeeded(Path) ||
            !AppendPathComponent(Path, PathCapacity, L"Logs") ||
            !CreateDirectoryIfNeeded(Path))
        {
            return false;
        }

        wchar_t FileName[64];
        if (FAILED(::StringCchPrintfW(
            FileName,
            ARRAYSIZE(FileName),
            L"NanaZip.ShellExtension.%lu.log",
            ::GetCurrentProcessId())))
        {
            return false;
        }

        return AppendPathComponent(Path, PathCapacity, FileName);
    }

    static bool GetLogFilePath(
        wchar_t* Path,
        size_t PathCapacity,
        bool AllowInitialize)
    {
        if (!Path || PathCapacity == 0)
        {
            return false;
        }

        if (g_LogFilePathState.load(std::memory_order_acquire) == 2)
        {
            return SUCCEEDED(::StringCchCopyW(
                Path,
                PathCapacity,
                g_LogFilePath));
        }

        if (!AllowInitialize)
        {
            return false;
        }

        long Expected = 0;
        if (g_LogFilePathState.compare_exchange_strong(
            Expected,
            1,
            std::memory_order_acq_rel))
        {
            wchar_t BuiltPath[MAX_PATH];
            if (TryBuildLogFilePath(BuiltPath, ARRAYSIZE(BuiltPath)) &&
                SUCCEEDED(::StringCchCopyW(
                    g_LogFilePath,
                    ARRAYSIZE(g_LogFilePath),
                    BuiltPath)))
            {
                g_LogFilePathState.store(2, std::memory_order_release);
                return SUCCEEDED(::StringCchCopyW(
                    Path,
                    PathCapacity,
                    BuiltPath));
            }

            g_LogFilePathState.store(0, std::memory_order_release);
            return false;
        }

        return TryBuildLogFilePath(Path, PathCapacity);
    }

    static void WriteLogLine(
        const wchar_t* Message,
        bool AllowLogFilePathInitialization)
    {
        if (!Message)
        {
            return;
        }

        wchar_t LogFilePath[MAX_PATH];
        if (!GetLogFilePath(
            LogFilePath,
            ARRAYSIZE(LogFilePath),
            AllowLogFilePathInitialization))
        {
            return;
        }

        HANDLE LogFile = ::CreateFileW(
            LogFilePath,
            FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (LogFile == INVALID_HANDLE_VALUE)
        {
            return;
        }

        LARGE_INTEGER FileSize = {};
        if (::GetFileSizeEx(LogFile, &FileSize) && FileSize.QuadPart == 0)
        {
            static const BYTE kUtf8ByteOrderMark[] = { 0xEF, 0xBB, 0xBF };
            DWORD BytesWritten = 0;
            ::WriteFile(
                LogFile,
                kUtf8ByteOrderMark,
                sizeof(kUtf8ByteOrderMark),
                &BytesWritten,
                nullptr);
        }

        char Utf8Message[4096];
        int BytesToWrite = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            Message,
            -1,
            Utf8Message,
            ARRAYSIZE(Utf8Message),
            nullptr,
            nullptr);
        if (BytesToWrite > 0)
        {
            DWORD BytesWritten = 0;
            ::WriteFile(
                LogFile,
                Utf8Message,
                static_cast<DWORD>(BytesToWrite - 1),
                &BytesWritten,
                nullptr);
        }

        ::CloseHandle(LogFile);
    }

    static void LogMessageCore(
        bool AllowLogFilePathInitialization,
        const wchar_t* Format,
        va_list Arguments)
    {
        try
        {
            wchar_t Body[3072];
            HRESULT Result = ::StringCchVPrintfW(
                Body,
                ARRAYSIZE(Body),
                Format,
                Arguments);
            if (FAILED(Result))
            {
                return;
            }

            SYSTEMTIME LocalTime = {};
            ::GetLocalTime(&LocalTime);

            wchar_t Line[4096];
            Result = ::StringCchPrintfW(
                Line,
                ARRAYSIZE(Line),
                L"%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu "
                L"pid=%lu tid=%lu age_ms=%llu %s\r\n",
                LocalTime.wYear,
                LocalTime.wMonth,
                LocalTime.wDay,
                LocalTime.wHour,
                LocalTime.wMinute,
                LocalTime.wSecond,
                LocalTime.wMilliseconds,
                ::GetCurrentProcessId(),
                ::GetCurrentThreadId(),
                GetProcessAgeMilliseconds(),
                Body);
            if (FAILED(Result))
            {
                return;
            }

            WriteLogLine(Line, AllowLogFilePathInitialization);
        }
        catch (...)
        {
        }
    }

    static void LogMessage(const wchar_t* Format, ...)
    {
        va_list Arguments;
        va_start(Arguments, Format);
        LogMessageCore(true, Format, Arguments);
        va_end(Arguments);
    }

    static void LogMessageWithoutLogFilePathInitialization(
        const wchar_t* Format,
        ...)
    {
        va_list Arguments;
        va_start(Arguments, Format);
        LogMessageCore(false, Format, Arguments);
        va_end(Arguments);
    }

    static void LogProcessAttachComProcessReferenceProbe()
    {
        bool Expected = false;
        if (!g_ProcessAttachComProcessRefLogged.compare_exchange_strong(
            Expected,
            true))
        {
            return;
        }

        const unsigned long AfterAdd =
            g_ProcessAttachComProcessRefAfterAdd.load();
        const unsigned long AfterRelease =
            g_ProcessAttachComProcessRefAfterRelease.load();
        if (AfterAdd == kComProcessReferenceProbeUnavailable ||
            AfterRelease == kComProcessReferenceProbeUnavailable)
        {
            LogMessage(
                L"lifecycle event=dll_process_attach_com_process_ref_probe "
                L"com_process_ref_probe=unavailable module_lock=%lld",
                GetCurrentModuleLockCount());
            return;
        }

        LogMessage(
            L"lifecycle event=dll_process_attach_com_process_ref_probe "
            L"com_process_ref_probe=%lu/%lu module_lock=%lld",
            AfterAdd,
            AfterRelease,
            GetCurrentModuleLockCount());
    }

    static void LogCurrentState(
        const wchar_t* Reason,
        bool AllowLogFilePathInitialization = true)
    {
        const long long BaseCreated =
            g_ExplorerCommandBaseCounters.Created.load();
        const long long BaseDestroyed =
            g_ExplorerCommandBaseCounters.Destroyed.load();
        const long long RootCreated =
            g_ExplorerCommandRootCounters.Created.load();
        const long long RootDestroyed =
            g_ExplorerCommandRootCounters.Destroyed.load();
        const long long FactoryCreated =
            g_ClassFactoryCounters.Created.load();
        const long long FactoryDestroyed =
            g_ClassFactoryCounters.Destroyed.load();

        if (AllowLogFilePathInitialization)
        {
            const ComServerProcessReferenceProbe ComProcessRef =
                ProbeComServerProcessReferenceForLog();
            LogMessage(
                L"state reason=\"%s\" module_lock=%lld "
                L"com_process_ref_probe=%lu/%lu "
                L"ExplorerCommandBase=%lld/%lld(balance=%lld) "
                L"ExplorerCommandRoot=%lld/%lld(balance=%lld) "
                L"ClassFactory=%lld/%lld(balance=%lld)",
                Reason ? Reason : L"",
                GetCurrentModuleLockCount(),
                ComProcessRef.AfterAdd,
                ComProcessRef.AfterRelease,
                BaseCreated,
                BaseDestroyed,
                BaseCreated - BaseDestroyed,
                RootCreated,
                RootDestroyed,
                RootCreated - RootDestroyed,
                FactoryCreated,
                FactoryDestroyed,
                FactoryCreated - FactoryDestroyed);
        }
        else
        {
            LogMessageWithoutLogFilePathInitialization(
                L"state reason=\"%s\" module_lock=%lld "
                L"com_process_ref_probe=skipped "
                L"ExplorerCommandBase=%lld/%lld(balance=%lld) "
                L"ExplorerCommandRoot=%lld/%lld(balance=%lld) "
                L"ClassFactory=%lld/%lld(balance=%lld)",
                Reason ? Reason : L"",
                GetCurrentModuleLockCount(),
                BaseCreated,
                BaseDestroyed,
                BaseCreated - BaseDestroyed,
                RootCreated,
                RootDestroyed,
                RootCreated - RootDestroyed,
                FactoryCreated,
                FactoryDestroyed,
                FactoryCreated - FactoryDestroyed);
        }
    }

    static void LogLifecycleEvent(
        const wchar_t* EventName,
        const wchar_t* Detail = L"")
    {
        LogMessage(
            L"lifecycle event=%s detail=\"%s\" module_lock=%lld",
            EventName ? EventName : L"",
            Detail ? Detail : L"",
            GetCurrentModuleLockCount());
    }

    static void LogLifecycleEventWithoutLogFilePathInitialization(
        const wchar_t* EventName,
        const wchar_t* Detail = L"")
    {
        LogMessageWithoutLogFilePathInitialization(
            L"lifecycle event=%s detail=\"%s\" module_lock=%lld",
            EventName ? EventName : L"",
            Detail ? Detail : L"",
            GetCurrentModuleLockCount());
    }

    static void LogLifecycleResult(
        const wchar_t* EventName,
        HRESULT Result)
    {
        LogMessage(
            L"lifecycle event=%s result=0x%08X module_lock=%lld",
            EventName ? EventName : L"",
            static_cast<unsigned int>(Result),
            GetCurrentModuleLockCount());
    }

    static void FormatGuidForLog(
        REFGUID Guid,
        wchar_t* Buffer,
        size_t BufferCapacity)
    {
        if (!Buffer || BufferCapacity == 0)
        {
            return;
        }

        if (::StringFromGUID2(Guid, Buffer, static_cast<int>(BufferCapacity)) == 0)
        {
            ::StringCchCopyW(Buffer, BufferCapacity, L"<failed>");
        }
    }

    static const wchar_t* GetKnownInterfaceNameForLog(REFGUID InterfaceId)
    {
        if (InterfaceId == IID_IUnknown)
        {
            return L"IUnknown";
        }

        if (InterfaceId == IID_IClassFactory)
        {
            return L"IClassFactory";
        }

        if (InterfaceId == __uuidof(IExplorerCommand))
        {
            return L"IExplorerCommand";
        }

        if (InterfaceId == __uuidof(IInitializeCommand))
        {
            return L"IInitializeCommand";
        }

        if (InterfaceId == __uuidof(IEnumExplorerCommand))
        {
            return L"IEnumExplorerCommand";
        }

        return L"<unknown>";
    }

    static void CALLBACK LongRunningReportTimerCallback(
        PVOID Parameter,
        BOOLEAN TimerOrWaitFired)
    {
        UNREFERENCED_PARAMETER(Parameter);
        UNREFERENCED_PARAMETER(TimerOrWaitFired);

        LogCurrentState(L"long-running process report");
    }

    static void StartLongRunningReportTimer()
    {
        bool Expected = false;
        if (!g_LongRunningReportTimerStarted.compare_exchange_strong(
            Expected,
            true))
        {
            return;
        }

        HANDLE Timer = nullptr;
        if (::CreateTimerQueueTimer(
            &Timer,
            nullptr,
            LongRunningReportTimerCallback,
            nullptr,
            kLongRunningReportDelayMilliseconds,
            kLongRunningReportPeriodMilliseconds,
            WT_EXECUTEDEFAULT))
        {
            g_LongRunningReportTimer.store(Timer);
        }
        else
        {
            g_LongRunningReportTimerStarted.store(false);
        }
    }

    static void StopLongRunningReportTimer(bool WaitForCallback)
    {
        HANDLE Timer = static_cast<HANDLE>(
            g_LongRunningReportTimer.exchange(nullptr));
        if (!Timer)
        {
            g_LongRunningReportTimerStarted.store(false);
            return;
        }

        HANDLE CompletionEvent = WaitForCallback ? INVALID_HANDLE_VALUE : nullptr;
        if (!::DeleteTimerQueueTimer(nullptr, Timer, CompletionEvent) &&
            ::GetLastError() != ERROR_IO_PENDING &&
            WaitForCallback)
        {
            LogMessage(
                L"timer delete failed error=%lu module_lock=%lld",
                ::GetLastError(),
                GetCurrentModuleLockCount());
        }

        g_LongRunningReportTimerStarted.store(false);
    }

    static ShellExtensionObjectCounters& GetObjectCounters(
        const wchar_t* ObjectName)
    {
        if (wcscmp(ObjectName, L"ExplorerCommandRoot") == 0)
        {
            return g_ExplorerCommandRootCounters;
        }

        if (wcscmp(ObjectName, L"ClassFactory") == 0)
        {
            return g_ClassFactoryCounters;
        }

        return g_ExplorerCommandBaseCounters;
    }

    static void LogObjectEvent(
        const wchar_t* ObjectName,
        const void* ObjectAddress,
        bool Created,
        DWORD CommandID = static_cast<DWORD>(-1),
        bool WriteEventLog = true)
    {
        StartLongRunningReportTimer();

        ShellExtensionObjectCounters& Counters = GetObjectCounters(ObjectName);
        long long CreatedCount = Counters.Created.load();
        long long DestroyedCount = Counters.Destroyed.load();
        if (Created)
        {
            CreatedCount = Counters.Created.fetch_add(1) + 1;
        }
        else
        {
            DestroyedCount = Counters.Destroyed.fetch_add(1) + 1;
        }

        if (WriteEventLog)
        {
            LogMessage(
                L"object event=%s type=%s this=%p command_id=%lu "
                L"module_lock=%lld created=%lld destroyed=%lld balance=%lld",
                Created ? L"created" : L"destroyed",
                ObjectName,
                ObjectAddress,
                CommandID,
                GetCurrentModuleLockCount(),
                CreatedCount,
                DestroyedCount,
                CreatedCount - DestroyedCount);
        }
    }

    struct ShellExtensionPublicFunctionScope
    {
        const wchar_t* FunctionName;
        const void* ObjectAddress;
        DWORD CommandID;
        int UncaughtExceptionCount;

        explicit ShellExtensionPublicFunctionScope(
            const wchar_t* functionName,
            const void* objectAddress = nullptr,
            DWORD commandID = static_cast<DWORD>(-1)) :
            FunctionName(functionName),
            ObjectAddress(objectAddress),
            CommandID(commandID),
            UncaughtExceptionCount(std::uncaught_exceptions())
        {
            StartLongRunningReportTimer();
            LogMessage(
                L"public interface enter function=%s this=%p command_id=%lu "
                L"module_lock=%lld",
                FunctionName,
                ObjectAddress,
                CommandID,
                GetCurrentModuleLockCount());
        }

        ~ShellExtensionPublicFunctionScope()
        {
            if (std::uncaught_exceptions() > UncaughtExceptionCount)
            {
                LogMessage(
                    L"public interface exception escaping function=%s "
                    L"module_lock=%lld",
                    FunctionName,
                    GetCurrentModuleLockCount());
                LogCurrentState(L"exception escaping public interface");
            }
            else
            {
                LogMessage(
                    L"public interface leave function=%s this=%p "
                    L"command_id=%lu module_lock=%lld",
                    FunctionName,
                    ObjectAddress,
                    CommandID,
                    GetCurrentModuleLockCount());
            }
        }
    };

    static HRESULT LogCurrentPublicInterfaceExceptionAndReturn(
        const wchar_t* FunctionName)
    {
        HRESULT Result = winrt::to_hresult();
        LogMessage(
            L"public interface exception caught function=%s hresult=0x%08X "
            L"module_lock=%lld",
            FunctionName,
            static_cast<unsigned int>(Result),
            GetCurrentModuleLockCount());
        LogCurrentState(L"exception caught in public interface");
        return Result;
    }

#define NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE(functionName) \
    ShellExtensionPublicFunctionScope PublicFunctionScope(functionName)

#define NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(functionName) \
    ShellExtensionPublicFunctionScope PublicFunctionScope(functionName, this)

#define NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_COMMAND(functionName) \
    ShellExtensionPublicFunctionScope PublicFunctionScope( \
        functionName, this, this->m_CommandID)

#define NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(functionName) \
    catch (...) \
    { \
        return LogCurrentPublicInterfaceExceptionAndReturn(functionName); \
    }
}

namespace NanaZip::ShellExtension
{
    namespace CommandID
    {
        enum
        {
            None,

            Open,
            Test,

            Extract,
            ExtractHere,
            ExtractHereSmart,
            ExtractTo,

            Compress,
            CompressTo7z,
            CompressToZip,

            CompressEmail,
            CompressTo7zEmail,
            CompressToZipEmail,

            HashCRC32,
            HashCRC64,
            HashSHA1,
            HashSHA256,
            HashAll,

            Maximum
        };
    }

    namespace CommandGuid
    {
        static const GUID Root =
        { 0x469d94e9, 0x6af4, 0x4395, { 0xb3, 0x96, 0x99, 0xb1, 0x30, 0x8f, 0x8c, 0xe5 } };

        static const GUID Values[CommandID::Maximum] =
        {
            { 0x00000000, 0x0000, 0x0000, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x01 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x02 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x03 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x04 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x05 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x06 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x07 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x08 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x09 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x0a } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x0b } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x0c } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x0d } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x0e } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x0f } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x10 } },
            { 0xb7e5a6e4, 0x8c25, 0x4d48, { 0x9d, 0x8f, 0x2b, 0x7d, 0x2d, 0x99, 0xe1, 0x11 } }
        };
    }

    static const GUID* GetCanonicalGuidForCommandID(DWORD commandID)
    {
        if (commandID == CommandID::None || commandID >= CommandID::Maximum)
        {
            return nullptr;
        }

        return &CommandGuid::Values[commandID];
    }

    using SubCommandList = std::vector<winrt::com_ptr<IExplorerCommand>>;
    using SubCommandListIterator = SubCommandList::const_iterator;

    struct ExplorerCommandBase : public winrt::implements<
        ExplorerCommandBase,
        IExplorerCommand>
    {
    private:

        std::wstring m_Title;

        DWORD m_CommandID;
        bool m_IsSeparator;
        CBoolPair m_ElimDup;
        UInt32 m_WriteZone;

    public:

        ExplorerCommandBase(
            std::wstring const& Title = std::wstring(),
            DWORD CommandID = CommandID::None,
            CBoolPair const& ElimDup = CBoolPair(),
            UInt32 const& WriteZone = static_cast<UInt32>(-1)) :
            m_Title(Title),
            m_CommandID(CommandID),
            m_ElimDup(ElimDup),
            m_WriteZone(WriteZone)
        {
            this->m_IsSeparator = (this->m_CommandID == CommandID::None);
            LogObjectEvent(
                L"ExplorerCommandBase",
                this,
                true,
                this->m_CommandID,
                false);
        }

        ~ExplorerCommandBase()
        {
            LogObjectEvent(
                L"ExplorerCommandBase",
                this,
                false,
                this->m_CommandID,
                false);
        }

#pragma region IExplorerCommand

        HRESULT STDMETHODCALLTYPE GetTitle(
            _In_opt_ IShellItemArray* psiItemArray,
            _Outptr_ LPWSTR* ppszName)
        {
            try
            {
                UNREFERENCED_PARAMETER(psiItemArray);

                if (this->m_IsSeparator)
                {
                    *ppszName = nullptr;
                    return S_FALSE;
                }

                return ::SHStrDupW(this->m_Title.c_str(), ppszName);
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandBase::GetTitle")
        }

        HRESULT STDMETHODCALLTYPE GetIcon(
            _In_opt_ IShellItemArray* psiItemArray,
            _Outptr_ LPWSTR* ppszIcon)
        {
            try
            {
                UNREFERENCED_PARAMETER(psiItemArray);

                *ppszIcon = nullptr;
                return E_NOTIMPL;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandBase::GetIcon")
        }

        HRESULT STDMETHODCALLTYPE GetToolTip(
            _In_opt_ IShellItemArray* psiItemArray,
            _Outptr_ LPWSTR* ppszInfotip)
        {
            try
            {
                UNREFERENCED_PARAMETER(psiItemArray);
                *ppszInfotip = nullptr;
                return E_NOTIMPL;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandBase::GetToolTip")
        }

        HRESULT STDMETHODCALLTYPE GetCanonicalName(
            _Out_ GUID* pguidCommandName)
        {
            try
            {
                const GUID* CommandGuid = GetCanonicalGuidForCommandID(
                    this->m_CommandID);
                if (!CommandGuid)
                {
                    *pguidCommandName = GUID_NULL;
                    LogMessage(
                        L"canonical name result type=ExplorerCommandBase "
                        L"this=%p command_id=%lu result=0x%08X "
                        L"reason=\"no command guid\" module_lock=%lld",
                        this,
                        this->m_CommandID,
                        static_cast<unsigned int>(E_NOTIMPL),
                        GetCurrentModuleLockCount());
                    return E_NOTIMPL;
                }

                *pguidCommandName = *CommandGuid;

                wchar_t GuidText[64];
                FormatGuidForLog(
                    *pguidCommandName,
                    GuidText,
                    ARRAYSIZE(GuidText));
                LogMessage(
                    L"canonical name result type=ExplorerCommandBase "
                    L"this=%p command_id=%lu result=0x%08X guid=%s "
                    L"module_lock=%lld",
                    this,
                    this->m_CommandID,
                    static_cast<unsigned int>(S_OK),
                    GuidText,
                    GetCurrentModuleLockCount());
                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandBase::GetCanonicalName")
        }

        HRESULT STDMETHODCALLTYPE GetState(
            _In_opt_ IShellItemArray* psiItemArray,
            _In_ BOOL fOkToBeSlow,
            _Out_ EXPCMDSTATE* pCmdState)
        {
            try
            {
                UNREFERENCED_PARAMETER(psiItemArray);
                UNREFERENCED_PARAMETER(fOkToBeSlow);
                *pCmdState = ECS_ENABLED;
                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandBase::GetState")
        }

        HRESULT STDMETHODCALLTYPE Invoke(
            _In_opt_ IShellItemArray* psiItemArray,
            _In_opt_ IBindCtx* pbc)
        {
            try
            {
                UNREFERENCED_PARAMETER(pbc);

                if (this->m_IsSeparator)
                {
                    return E_NOTIMPL;
                }

            std::vector<std::wstring> FilePaths;
            if (psiItemArray)
            {
                DWORD Count = 0;
                if (SUCCEEDED(psiItemArray->GetCount(&Count)))
                {
                    for (DWORD i = 0; i < Count; ++i)
                    {
                        winrt::com_ptr<IShellItem> Item;
                        if (SUCCEEDED(psiItemArray->GetItemAt(
                            i,
                            Item.put())))
                        {
                            LPWSTR DisplayName = nullptr;
                            if (SUCCEEDED(Item->GetDisplayName(
                                SIGDN_FILESYSPATH,
                                &DisplayName)))
                            {
                                FilePaths.push_back(std::wstring(DisplayName));
                                ::CoTaskMemFree(DisplayName);
                            }
                        }
                    }
                }
            }

            bool NeedExtract = false;
            if (FilePaths.size() > 0)
            {
                for (std::wstring const FilePath : FilePaths)
                {
                    DWORD FileAttributes = ::GetFileAttributesW(
                        FilePath.c_str());
                    if (FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        continue;
                    }

                    if (DoNeedExtract(::PathFindFileNameW(FilePath.c_str())))
                    {
                        NeedExtract = true;
                        break;
                    }
                }
            }

            std::wstring SpecFolder = L"*";
            if (NeedExtract)
            {
                if (FilePaths.size() == 1)
                {
                    SpecFolder = GetSubFolderNameForExtract(
                        ::PathFindFileNameW(FilePaths[0].c_str()));
                }
                SpecFolder += L'\\';
            }

            UStringVector FileNames;
            for (std::wstring const FilePath : FilePaths)
            {
                FileNames.Add(FilePath.c_str());
            }

            FString FolderPrefix;

            std::wstring ArchiveName;
            if (FilePaths.size() > 0)
            {
                NWindows::NFile::NFind::CFileInfo FileInfo0;

                const UString& FileName = FileNames.Front();

                if (NWindows::NFile::NName::IsDevicePath(us2fs(FileName)))
                {
                    // CFileInfo::Find can be slow for device files. So we
                    // don't call it.
                    // we need only name here.
                    // change it 4 - must be constant
                    FileInfo0.Name = us2fs(FileName.Ptr(
                        NWindows::NFile::NName::kDevicePathPrefixSize));
                    FolderPrefix = "C:\\";
                }
                else
                {
                    if (!FileInfo0.Find(us2fs(FileName)))
                    {
                        return ::HRESULT_FROM_WIN32(::GetLastError());
                    }
                    NWindows::NFile::NDir::GetOnlyDirPrefix(
                        us2fs(FileName),
                        FolderPrefix);
                }

                const UString Name = CreateArchiveName(
                    FileNames,
                    FileNames.Size() == 1 ? &FileInfo0 : nullptr);
                ArchiveName = std::wstring(Name.Ptr(), Name.Len());

            }

            std::wstring BaseFolder = std::wstring(
                FolderPrefix.Ptr(),
                FolderPrefix.Len());

            std::wstring ArchiveName7z = ArchiveName + L".7z";
            std::wstring ArchiveNameZip = ArchiveName + L".zip";

            LogMessage(
                L"command invoke dispatch this=%p command_id=%lu "
                L"file_count=%llu need_extract=%u module_lock=%lld",
                this,
                this->m_CommandID,
                static_cast<unsigned long long>(FilePaths.size()),
                NeedExtract ? 1u : 0u,
                GetCurrentModuleLockCount());

            switch (this->m_CommandID)
            {
            case CommandID::Open:
            {
                if (FilePaths.size() != 1)
                {
                    LogLifecycleEvent(
                        L"open_command_skip",
                        L"file count is not one");
                    break;
                }

                DWORD FileAttributes = ::GetFileAttributesW(
                    FilePaths[0].c_str());
                if (FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    LogLifecycleEvent(
                        L"open_command_skip",
                        L"target is directory");
                    break;
                }

                if (!DoNeedExtract(FilePaths[0].c_str()))
                {
                    LogLifecycleEvent(
                        L"open_command_skip",
                        L"target does not need extract");
                    break;
                }

                UString params;
                params = GetQuotedString(FilePaths[0].c_str());
                WRes Result = NWindows::MyCreateProcess(
                    ::GetNanaZipPath(),
                    params);
                LogMessage(
                    L"lifecycle event=open_command_process_create_return "
                    L"result=%u module_lock=%lld",
                    static_cast<unsigned int>(Result),
                    GetCurrentModuleLockCount());

                break;
            }
            case CommandID::Test:
            {
                if (!NeedExtract)
                {
                    break;
                }

                TestArchives(FileNames);
                break;
            }
            case CommandID::Extract:
            case CommandID::ExtractHere:
            case CommandID::ExtractHereSmart:
            case CommandID::ExtractTo:
            {
                if (!NeedExtract)
                {
                    break;
                }

                std::wstring Folder = BaseFolder;
                if (this->m_CommandID != CommandID::ExtractHere &&
                    this->m_CommandID != CommandID::ExtractHereSmart)
                {
                    Folder += SpecFolder;
                }

                ExtractArchives(
                    FileNames,
                    Folder.c_str(),
                    (this->m_CommandID == CommandID::Extract),
                    ((this->m_CommandID == CommandID::ExtractTo)
                    && this->m_ElimDup.Val),
                    this->m_WriteZone,
                    (this->m_CommandID == CommandID::ExtractHereSmart));

                break;
            }
            case CommandID::Compress:
            case CommandID::CompressTo7z:
            case CommandID::CompressToZip:
            case CommandID::CompressEmail:
            case CommandID::CompressTo7zEmail:
            case CommandID::CompressToZipEmail:
            {
                bool Email =(
                    (this->m_CommandID == CommandID::CompressEmail) ||
                    (this->m_CommandID == CommandID::CompressTo7zEmail) ||
                    (this->m_CommandID == CommandID::CompressToZipEmail));
                bool ShowDialog = (
                    (this->m_CommandID == CommandID::Compress) ||
                    (this->m_CommandID == CommandID::CompressEmail));
                bool AddExtension = (
                    (this->m_CommandID == CommandID::Compress) ||
                    (this->m_CommandID == CommandID::CompressEmail));
                bool Is7z = (
                    (this->m_CommandID == CommandID::CompressTo7z) ||
                    (this->m_CommandID == CommandID::CompressTo7zEmail));

                std::wstring Name = (
                    AddExtension
                    ? ArchiveName
                    : (Is7z ? ArchiveName7z : ArchiveNameZip));

                CompressFiles(
                    BaseFolder.c_str(),
                    Name.c_str(),
                    Is7z ? L"7z" : L"zip",
                    AddExtension,
                    FileNames,
                    Email,
                    ShowDialog,
                    false);

                break;
            }
            case CommandID::HashCRC32:
            case CommandID::HashCRC64:
            case CommandID::HashSHA1:
            case CommandID::HashSHA256:
            case CommandID::HashAll:
            {
                std::wstring MethodName;
                switch (this->m_CommandID)
                {
                case CommandID::HashCRC32:
                    MethodName = L"CRC32";
                    break;
                case CommandID::HashCRC64:
                    MethodName = L"CRC64";
                    break;
                case CommandID::HashSHA1:
                    MethodName = L"SHA1";
                    break;
                case CommandID::HashSHA256:
                    MethodName = L"SHA256";
                    break;
                case CommandID::HashAll:
                    MethodName = L"*";
                    break;
                default:
                    break;
                }

                CalcChecksum(FileNames, MethodName.c_str(), L"", L"");
                break;
            }
            default:
                break;
            }

                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandBase::Invoke")
        }

        HRESULT STDMETHODCALLTYPE GetFlags(
            _Out_ EXPCMDFLAGS* pFlags)
        {
            try
            {
                *pFlags =
                    this->m_IsSeparator
                    ? ECF_ISSEPARATOR
                    : ECF_DEFAULT;
                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandBase::GetFlags")
        }

        HRESULT STDMETHODCALLTYPE EnumSubCommands(
            _Outptr_ IEnumExplorerCommand** ppEnum)
        {
            try
            {
                *ppEnum = nullptr;
                return E_NOTIMPL;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandBase::EnumSubCommands")
        }

#pragma endregion

    };


    struct ExplorerCommandRoot : public winrt::implements<
        ExplorerCommandRoot,
        IExplorerCommand,
        IEnumExplorerCommand,
        IInitializeCommand,
        winrt::non_agile>
    {
    private:

        DWORD m_ContextMenuFlags;
        CBoolPair m_ContextMenuElimDup;

        bool m_Initialized = false;
        SubCommandList m_SubCommands;
        SubCommandListIterator m_CurrentSubCommand;

        void Initialize(
            _In_opt_ IShellItemArray* psiItemArray)
        {
            if (m_Initialized)
            {
                return;
            }

            m_Initialized = true;
            LogLifecycleEvent(L"root_initialize_enter");

            std::vector<std::wstring> FilePaths;
            if (psiItemArray)
            {
                DWORD Count = 0;
                if (SUCCEEDED(psiItemArray->GetCount(&Count)))
                {
                    for (DWORD i = 0; i < Count; ++i)
                    {
                        winrt::com_ptr<IShellItem> Item;
                        if (SUCCEEDED(psiItemArray->GetItemAt(
                            i,
                            Item.put())))
                        {
                            LPWSTR DisplayName = nullptr;
                            if (SUCCEEDED(Item->GetDisplayName(
                                SIGDN_FILESYSPATH,
                                &DisplayName)))
                            {
                                FilePaths.push_back(std::wstring(DisplayName));
                                ::CoTaskMemFree(DisplayName);
                            }
                        }
                    }
                }
            }

            if (FilePaths.empty())
            {
                LogLifecycleEvent(
                    L"root_initialize_return",
                    L"no file paths");
                return;
            }

            UStringVector FileNames;
            for (std::wstring const FilePath : FilePaths)
            {
                FileNames.Add(FilePath.c_str());
            }

            bool NeedExtract = false;
            for (std::wstring const FilePath : FilePaths)
            {
                DWORD FileAttributes = ::GetFileAttributesW(
                    FilePath.c_str());
                if (FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }

                if (DoNeedExtract(::PathFindFileNameW(FilePath.c_str())))
                {
                    NeedExtract = true;
                    break;
                }
            }

            std::wstring SpecFolder = L"*";
            if (NeedExtract)
            {
                if (FilePaths.size() == 1)
                {
                    SpecFolder = GetSubFolderNameForExtract(
                        ::PathFindFileNameW(FilePaths[0].c_str()));
                }
                SpecFolder += L'\\';
            }

            FString FolderPrefix;

            std::wstring ArchiveName;
            {
                NWindows::NFile::NFind::CFileInfo FileInfo0;

                const UString& FileName = FileNames.Front();

                if (NWindows::NFile::NName::IsDevicePath(us2fs(FileName)))
                {
                    // CFileInfo::Find can be slow for device files. So we
                    // don't call it.
                    // we need only name here.
                    // change it 4 - must be constant
                    FileInfo0.Name = us2fs(FileName.Ptr(
                        NWindows::NFile::NName::kDevicePathPrefixSize));
                    FolderPrefix = "C:\\";
                }
                else
                {
                    if (!FileInfo0.Find(us2fs(FileName)))
                    {
                        LogLifecycleEvent(
                            L"root_initialize_return",
                            L"file info unavailable");
                        return;
                    }
                    NWindows::NFile::NDir::GetOnlyDirPrefix(
                        us2fs(FileName),
                        FolderPrefix);
                }

                const UString Name = CreateArchiveName(
                    FileNames,
                    FileNames.Size() == 1 ? &FileInfo0 : nullptr);
                ArchiveName = std::wstring(Name.Ptr(), Name.Len());

            }

            std::wstring BaseFolder = std::wstring(
                FolderPrefix.Ptr(),
                FolderPrefix.Len());

            std::wstring ArchiveName7z = ArchiveName + L".7z";
            std::wstring ArchiveNameZip = ArchiveName + L".zip";

            using NanaZip::ShellExtension::ExplorerCommandBase;

            CContextMenuInfo ContextMenuInfo;
            ContextMenuInfo.Load();
            DWORD ContextMenuFlags = ContextMenuInfo.Flags;
            CBoolPair ContextMenuElimDup = ContextMenuInfo.ElimDup;
            UInt32 ContextMenuWriteZone = ContextMenuInfo.WriteZone;

            LoadLangOneTime();

            if (ContextMenuFlags & NContextMenuFlags::kOpen)
            {
                DWORD FileAttributes = ::GetFileAttributesW(
                    FilePaths[0].c_str());
                if ((FilePaths.size() == 1) &&
                    !(FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    DoNeedExtract(FilePaths[0].c_str()))
                {
                    UString TranslatedString;
                    LangString(IDS_CONTEXT_OPEN, TranslatedString);
                    this->m_SubCommands.push_back(
                        winrt::make<ExplorerCommandBase>(
                            std::wstring(
                                TranslatedString.Ptr(),
                                TranslatedString.Len()),
                            CommandID::Open));
                }
            }

            if (NeedExtract)
            {
                if (ContextMenuFlags & NContextMenuFlags::kTest)
                {
                    UString TranslatedString;
                    LangString(IDS_CONTEXT_TEST, TranslatedString);
                    this->m_SubCommands.push_back(
                        winrt::make<ExplorerCommandBase>(
                            std::wstring(
                                TranslatedString.Ptr(),
                                TranslatedString.Len()),
                            CommandID::Test));
                }

                if (ContextMenuFlags & NContextMenuFlags::kExtract)
                {
                    UString TranslatedString;
                    LangString(IDS_CONTEXT_EXTRACT, TranslatedString);
                    this->m_SubCommands.push_back(
                        winrt::make<ExplorerCommandBase>(
                            std::wstring(
                                TranslatedString.Ptr(),
                                TranslatedString.Len()),
                            CommandID::Extract,
                            ContextMenuElimDup,
                            ContextMenuWriteZone));
                }

                if (ContextMenuFlags & NContextMenuFlags::kExtractHere)
                {
                    UString TranslatedString;
                    LangString(IDS_CONTEXT_EXTRACT_HERE, TranslatedString);
                    this->m_SubCommands.push_back(
                        winrt::make<ExplorerCommandBase>(
                            std::wstring(
                                TranslatedString.Ptr(),
                                TranslatedString.Len()),
                            CommandID::ExtractHere,
                            ContextMenuElimDup,
                            ContextMenuWriteZone));
                }

                if (ContextMenuFlags & NContextMenuFlags::kExtractHereSmart)
                {
                    UString TranslatedString;
                    LangString(IDS_CONTEXT_EXTRACT_HERE_SMART, TranslatedString);
                    this->m_SubCommands.push_back(
                        winrt::make<ExplorerCommandBase>(
                            std::wstring(
                                TranslatedString.Ptr(),
                                TranslatedString.Len()),
                            CommandID::ExtractHereSmart,
                            ContextMenuElimDup,
                            ContextMenuWriteZone));
                }

                if (ContextMenuFlags & NContextMenuFlags::kExtractTo)
                {
                    UString TranslatedString;
                    LangString(IDS_CONTEXT_EXTRACT_TO, TranslatedString);
                    MyFormatNew_ReducedName(TranslatedString, SpecFolder.c_str());
                    this->m_SubCommands.push_back(
                        winrt::make<ExplorerCommandBase>(
                            std::wstring(
                                TranslatedString.Ptr(),
                                TranslatedString.Len()),
                            CommandID::ExtractTo,
                            ContextMenuElimDup,
                            ContextMenuWriteZone));
                }
            }

            if (ContextMenuFlags & NContextMenuFlags::kCompress)
            {
                UString TranslatedString;
                LangString(IDS_CONTEXT_COMPRESS, TranslatedString);
                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        std::wstring(
                            TranslatedString.Ptr(),
                            TranslatedString.Len()),
                        CommandID::Compress));
            }

            if (ContextMenuFlags & NContextMenuFlags::kCompressTo7z)
            {
                UString TranslatedString;
                LangString(IDS_CONTEXT_COMPRESS_TO, TranslatedString);
                MyFormatNew_ReducedName(TranslatedString, ArchiveName7z.c_str());
                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        std::wstring(
                            TranslatedString.Ptr(),
                            TranslatedString.Len()),
                        CommandID::CompressTo7z));
            }

            if (ContextMenuFlags & NContextMenuFlags::kCompressToZip)
            {
                UString TranslatedString;
                LangString(IDS_CONTEXT_COMPRESS_TO, TranslatedString);
                MyFormatNew_ReducedName(TranslatedString, ArchiveNameZip.c_str());
                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        std::wstring(
                            TranslatedString.Ptr(),
                            TranslatedString.Len()),
                        CommandID::CompressToZip));
            }

            if (ContextMenuFlags & NContextMenuFlags::kCompressEmail)
            {
                UString TranslatedString;
                LangString(IDS_CONTEXT_COMPRESS_EMAIL, TranslatedString);
                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        std::wstring(
                            TranslatedString.Ptr(),
                            TranslatedString.Len()),
                        CommandID::CompressEmail));
            }

            if (ContextMenuFlags & NContextMenuFlags::kCompressTo7zEmail)
            {
                UString TranslatedString;
                LangString(IDS_CONTEXT_COMPRESS_TO_EMAIL, TranslatedString);
                MyFormatNew_ReducedName(TranslatedString, ArchiveName7z.c_str());
                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        std::wstring(
                            TranslatedString.Ptr(),
                            TranslatedString.Len()),
                        CommandID::CompressTo7zEmail));
            }

            if (ContextMenuFlags & NContextMenuFlags::kCompressToZipEmail)
            {
                UString TranslatedString;
                LangString(IDS_CONTEXT_COMPRESS_TO_EMAIL, TranslatedString);
                MyFormatNew_ReducedName(TranslatedString, ArchiveNameZip.c_str());
                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        std::wstring(
                            TranslatedString.Ptr(),
                            TranslatedString.Len()),
                        CommandID::CompressToZipEmail));
            }

            if (ContextMenuFlags & NContextMenuFlags::kCRC)
            {
                if (!this->m_SubCommands.empty())
                {
                    this->m_SubCommands.push_back(
                        winrt::make<ExplorerCommandBase>());
                }

                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        L"CRC-32",
                        CommandID::HashCRC32));

                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        L"CRC-64",
                        CommandID::HashCRC64));

                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        L"SHA-1",
                        CommandID::HashSHA1));

                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        L"SHA-256",
                        CommandID::HashSHA256));

                this->m_SubCommands.push_back(
                    winrt::make<ExplorerCommandBase>(
                        L"*",
                        CommandID::HashAll));
            }

            LogMessage(
                L"lifecycle event=root_initialize_return detail=\"completed\" "
                L"subcommands=%llu need_extract=%u module_lock=%lld",
                static_cast<unsigned long long>(this->m_SubCommands.size()),
                NeedExtract ? 1u : 0u,
                GetCurrentModuleLockCount());
        }

    public:

        HRESULT STDMETHODCALLTYPE QueryInterface(
            _In_ REFIID riid,
            _COM_Outptr_ void** ppvObject) noexcept override
        {
            wchar_t InterfaceId[64];
            FormatGuidForLog(riid, InterfaceId, ARRAYSIZE(InterfaceId));
            HRESULT Result = root_implements_type::QueryInterface(
                riid,
                ppvObject);

            try
            {
                LogMessage(
                    L"lifecycle event=root_query_interface_return "
                    L"this=%p riid=%s riid_name=%s result=0x%08X "
                    L"returned_object=%p module_lock=%lld",
                    this,
                    InterfaceId,
                    GetKnownInterfaceNameForLog(riid),
                    static_cast<unsigned int>(Result),
                    ppvObject ? *ppvObject : nullptr,
                    GetCurrentModuleLockCount());
            }
            catch (...)
            {
            }

            return Result;
        }

        ULONG STDMETHODCALLTYPE AddRef() noexcept override
        {
            ULONG Result = root_implements_type::AddRef();
            try
            {
                LogMessage(
                    L"lifecycle event=root_add_ref_return this=%p "
                    L"ref_count=%lu module_lock=%lld",
                    this,
                    Result,
                    GetCurrentModuleLockCount());
            }
            catch (...)
            {
            }

            return Result;
        }

        ULONG STDMETHODCALLTYPE Release() noexcept override
        {
            void* This = this;
            try
            {
                LogMessage(
                    L"lifecycle event=root_release_enter this=%p "
                    L"module_lock=%lld",
                    This,
                    GetCurrentModuleLockCount());
            }
            catch (...)
            {
            }

            ULONG Result = root_implements_type::Release();
            try
            {
                LogMessage(
                    L"lifecycle event=root_release_return this=%p "
                    L"ref_count=%lu module_lock=%lld",
                    This,
                    Result,
                    GetCurrentModuleLockCount());
            }
            catch (...)
            {
            }

            return Result;
        }

        ExplorerCommandRoot()
        {
            CContextMenuInfo ContextMenuInfo;
            ContextMenuInfo.Load();
            this->m_ContextMenuFlags = ContextMenuInfo.Flags;
            this->m_ContextMenuElimDup = ContextMenuInfo.ElimDup;
            LogObjectEvent(L"ExplorerCommandRoot", this, true);
        }

        ~ExplorerCommandRoot()
        {
            LogObjectEvent(L"ExplorerCommandRoot", this, false);
        }

#pragma region IExplorerCommand

        HRESULT STDMETHODCALLTYPE GetTitle(
            _In_opt_ IShellItemArray* psiItemArray,
            _Outptr_ LPWSTR* ppszName)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::GetTitle");
            try
            {
                this->Initialize(psiItemArray);

                if (this->m_SubCommands.empty())
                {
                    *ppszName = nullptr;
                    return E_NOTIMPL;
                }

                return ::SHStrDupW(L"NanaZip Preview", ppszName);
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::GetTitle")
        }

        HRESULT STDMETHODCALLTYPE GetIcon(
            _In_opt_ IShellItemArray* psiItemArray,
            _Outptr_ LPWSTR* ppszIcon)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::GetIcon");
            try
            {
                UNREFERENCED_PARAMETER(psiItemArray);
                UString Path = ::GetNanaZipPath();
                std::wstring Icon = std::wstring(Path.Ptr(), Path.Len());
                Icon += L",-1";
                return ::SHStrDupW(Icon.c_str(), ppszIcon);
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::GetIcon")
        }

        HRESULT STDMETHODCALLTYPE GetToolTip(
            _In_opt_ IShellItemArray* psiItemArray,
            _Outptr_ LPWSTR* ppszInfotip)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::GetToolTip");
            try
            {
                UNREFERENCED_PARAMETER(psiItemArray);
                *ppszInfotip = nullptr;
                return E_NOTIMPL;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::GetToolTip")
        }

        HRESULT STDMETHODCALLTYPE GetCanonicalName(
            _Out_ GUID* pguidCommandName)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::GetCanonicalName");
            try
            {
                *pguidCommandName = CommandGuid::Root;

                wchar_t GuidText[64];
                FormatGuidForLog(
                    *pguidCommandName,
                    GuidText,
                    ARRAYSIZE(GuidText));
                LogMessage(
                    L"canonical name result type=ExplorerCommandRoot "
                    L"this=%p result=0x%08X guid=%s module_lock=%lld",
                    this,
                    static_cast<unsigned int>(S_OK),
                    GuidText,
                    GetCurrentModuleLockCount());
                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::GetCanonicalName")
        }

        HRESULT STDMETHODCALLTYPE GetState(
            _In_opt_ IShellItemArray* psiItemArray,
            _In_ BOOL fOkToBeSlow,
            _Out_ EXPCMDSTATE* pCmdState)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::GetState");
            try
            {
                UNREFERENCED_PARAMETER(psiItemArray);
                UNREFERENCED_PARAMETER(fOkToBeSlow);
                *pCmdState = ECS_ENABLED;
                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::GetState")
        }

        HRESULT STDMETHODCALLTYPE Invoke(
            _In_opt_ IShellItemArray* psiItemArray,
            _In_opt_ IBindCtx* pbc)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::Invoke");
            try
            {
                UNREFERENCED_PARAMETER(psiItemArray);
                UNREFERENCED_PARAMETER(pbc);
                return E_NOTIMPL;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::Invoke")
        }

        HRESULT STDMETHODCALLTYPE GetFlags(
            _Out_ EXPCMDFLAGS* pFlags)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::GetFlags");
            try
            {
                *pFlags = ECF_HASSUBCOMMANDS;
                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::GetFlags")
        }

        HRESULT STDMETHODCALLTYPE EnumSubCommands(
            _Outptr_ IEnumExplorerCommand** ppEnum)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::EnumSubCommands");
            try
            {
                LogMessage(
                    L"lifecycle event=enum_subcommands_enter "
                    L"subcommands=%llu module_lock=%lld",
                    static_cast<unsigned long long>(
                        this->m_SubCommands.size()),
                    GetCurrentModuleLockCount());

                if (this->m_SubCommands.empty())
                {
                    *ppEnum = nullptr;
                    LogLifecycleResult(
                        L"enum_subcommands_return",
                        E_NOTIMPL);
                    return E_NOTIMPL;
                }
                else
                {
                    this->m_CurrentSubCommand = this->m_SubCommands.cbegin();
                    HRESULT Result = this->QueryInterface(
                        IID_PPV_ARGS(ppEnum));
                    LogLifecycleResult(
                        L"enum_subcommands_return",
                        Result);
                    return Result;
                }
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::EnumSubCommands")
        }

#pragma endregion

#pragma region IEnumExplorerCommand

        HRESULT STDMETHODCALLTYPE Next(
            _In_ ULONG celt,
            _Out_ IExplorerCommand** pUICommand,
            _Out_opt_ ULONG* pceltFetched)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::Next");
            try
            {
                ULONG Fetched = 0;

                for (
                    ULONG i = 0;
                    (i < celt) &&
                    (this->m_CurrentSubCommand != this->m_SubCommands.cend());
                    ++i)
                {
                    this->m_CurrentSubCommand->copy_to(&pUICommand[i]);
                    ++Fetched;
                    ++this->m_CurrentSubCommand;
                }

                if (pceltFetched)
                {
                    *pceltFetched = Fetched;
                }

                return (Fetched == celt) ? S_OK : S_FALSE;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::Next")
        }

        HRESULT STDMETHODCALLTYPE Skip(
            _In_ ULONG celt)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::Skip");
            try
            {
                UNREFERENCED_PARAMETER(celt);
                return E_NOTIMPL;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::Skip")
        }

        HRESULT STDMETHODCALLTYPE Reset()
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::Reset");
            try
            {
                this->m_CurrentSubCommand = this->m_SubCommands.cbegin();
                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::Reset")
        }

        HRESULT STDMETHODCALLTYPE Clone(
            _Out_ IEnumExplorerCommand** ppenum)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::Clone");
            try
            {
                *ppenum = nullptr;
                return E_NOTIMPL;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::Clone")
        }

#pragma endregion

#pragma region IInitializeCommand

        HRESULT STDMETHODCALLTYPE Initialize(
            _In_ PCWSTR pszCommandName,
            _In_ IPropertyBag* ppb)
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ExplorerCommandRoot::Initialize");
            try
            {
                LogMessage(
                    L"lifecycle event=root_initialize_command "
                    L"this=%p command_name=\"%s\" property_bag=%p "
                    L"module_lock=%lld",
                    this,
                    pszCommandName ? pszCommandName : L"",
                    ppb,
                    GetCurrentModuleLockCount());
                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ExplorerCommandRoot::Initialize")
        }

#pragma endregion

    };

    struct DECLSPEC_UUID("469D94E9-6AF4-4395-B396-99B1308F8CE5")
        ClassFactory : public winrt::implements<
        ClassFactory, IClassFactory>
    {
    public:

        ClassFactory()
        {
            LogObjectEvent(L"ClassFactory", this, true);
        }

        ~ClassFactory()
        {
            LogObjectEvent(L"ClassFactory", this, false);
        }

        HRESULT STDMETHODCALLTYPE CreateInstance(
            _In_opt_ IUnknown* pUnkOuter,
            _In_ REFIID riid,
            _COM_Outptr_ void** ppvObject) noexcept override
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ClassFactory::CreateInstance");

            try
            {
                wchar_t InterfaceId[64];
                FormatGuidForLog(riid, InterfaceId, ARRAYSIZE(InterfaceId));
                const ComServerProcessReferenceProbe RequestComProcessRef =
                    ProbeComServerProcessReferenceForLog();
                LogMessage(
                    L"lifecycle event=class_factory_create_instance_request "
                    L"this=%p outer=%p riid=%s riid_name=%s ppv=%p "
                    L"module_lock=%lld com_process_ref_probe=%lu/%lu",
                    this,
                    pUnkOuter,
                    InterfaceId,
                    GetKnownInterfaceNameForLog(riid),
                    ppvObject,
                    GetCurrentModuleLockCount(),
                    RequestComProcessRef.AfterAdd,
                    RequestComProcessRef.AfterRelease);

                LogLifecycleEvent(L"class_factory_create_instance_enter");
                HRESULT Result = winrt::make<ExplorerCommandRoot>()->QueryInterface(
                    riid, ppvObject);
                const ComServerProcessReferenceProbe ResultComProcessRef =
                    ProbeComServerProcessReferenceForLog();
                LogMessage(
                    L"lifecycle event=class_factory_create_instance_result "
                    L"result=0x%08X returned_object=%p module_lock=%lld "
                    L"com_process_ref_probe=%lu/%lu",
                    static_cast<unsigned int>(Result),
                    ppvObject ? *ppvObject : nullptr,
                    GetCurrentModuleLockCount(),
                    ResultComProcessRef.AfterAdd,
                    ResultComProcessRef.AfterRelease);
                LogLifecycleResult(
                    L"class_factory_create_instance_return",
                    Result);
                return Result;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ClassFactory::CreateInstance")
        }

        HRESULT STDMETHODCALLTYPE LockServer(
            _In_ BOOL fLock) noexcept override
        {
            NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE_THIS(
                L"ClassFactory::LockServer");
            try
            {
                const long long LockCountBefore = GetCurrentModuleLockCount();
                if (fLock)
                {
                    ++winrt::get_module_lock();
                }
                else
                {
                    --winrt::get_module_lock();
                }

                LogMessage(
                    L"lockserver lock=%u module_lock_before=%lld "
                    L"module_lock_after=%lld",
                    static_cast<unsigned int>(fLock),
                    LockCountBefore,
                    GetCurrentModuleLockCount());

                return S_OK;
            }
            NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(
                L"ClassFactory::LockServer")
        }
    };
}

EXTERN_C HRESULT STDAPICALLTYPE DllCanUnloadNow()
{
    NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE(L"DllCanUnloadNow");
    try
    {
        LogLifecycleEvent(L"dll_can_unload_now_enter");
        if (winrt::get_module_lock())
        {
            LogCurrentState(L"DllCanUnloadNow returning S_FALSE");
            LogLifecycleResult(L"dll_can_unload_now_return", S_FALSE);
            return S_FALSE;
        }

        LogCurrentState(L"DllCanUnloadNow returning S_OK");
        LogLifecycleResult(L"dll_can_unload_now_return", S_OK);
        StopLongRunningReportTimer(true);
        winrt::clear_factory_cache();
        return S_OK;
    }
    NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(L"DllCanUnloadNow")
}

EXTERN_C HRESULT STDAPICALLTYPE DllGetClassObject(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _Outptr_ LPVOID* ppv)
{
    NANAZIP_SHELL_EXTENSION_PUBLIC_SCOPE(L"DllGetClassObject");
    try
    {
        LogProcessAttachComProcessReferenceProbe();
        LogLifecycleEvent(L"dll_get_class_object_enter");
        wchar_t ClassId[64];
        wchar_t InterfaceId[64];
        FormatGuidForLog(rclsid, ClassId, ARRAYSIZE(ClassId));
        FormatGuidForLog(riid, InterfaceId, ARRAYSIZE(InterfaceId));
        LogMessage(
            L"lifecycle event=dll_get_class_object_request "
            L"rclsid=%s riid=%s riid_name=%s ppv=%p module_lock=%lld",
            ClassId,
            InterfaceId,
            GetKnownInterfaceNameForLog(riid),
            ppv,
            GetCurrentModuleLockCount());
        if (!ppv)
        {
            LogLifecycleResult(L"dll_get_class_object_return", E_POINTER);
            return E_POINTER;
        }

        if (riid != IID_IClassFactory && riid != IID_IUnknown)
        {
            LogLifecycleResult(L"dll_get_class_object_return", E_NOINTERFACE);
            return E_NOINTERFACE;
        }

        if (rclsid != __uuidof(NanaZip::ShellExtension::ClassFactory))
        {
            LogLifecycleResult(L"dll_get_class_object_return", E_INVALIDARG);
            return E_INVALIDARG;
        }

        HRESULT Result = winrt::make<NanaZip::ShellExtension::ClassFactory>(
            )->QueryInterface(riid, ppv);
        LogMessage(
            L"lifecycle event=dll_get_class_object_result "
            L"result=0x%08X returned_object=%p module_lock=%lld",
            static_cast<unsigned int>(Result),
            ppv ? *ppv : nullptr,
            GetCurrentModuleLockCount());
        LogLifecycleResult(L"dll_get_class_object_return", Result);
        return Result;
    }
    NANAZIP_SHELL_EXTENSION_PUBLIC_CATCH(L"DllGetClassObject")
}

long g_DllRefCount = 0;
HWND g_HWND = nullptr;
HINSTANCE g_hInstance = nullptr;

// Only used in Shell Extension to check whether NanaZip Modern is loaded.
EXTERN_C HMODULE K7ModernCurrentModule;

BOOL WINAPI DllMain(
    _In_ HINSTANCE hinstDLL,
    _In_ DWORD fdwReason,
    _In_ LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);

    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
    {
        g_ProcessStartTick.store(::GetTickCount64());
        const ComServerProcessReferenceProbe ComProcessRef =
            ProbeComServerProcessReferenceForLog();
        g_ProcessAttachComProcessRefAfterAdd.store(ComProcessRef.AfterAdd);
        g_ProcessAttachComProcessRefAfterRelease.store(
            ComProcessRef.AfterRelease);
        g_hInstance = hinstDLL;
        ::K7ModernCurrentModule = hinstDLL;
        break;
    }
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        LogLifecycleEventWithoutLogFilePathInitialization(
            L"dll_process_detach_enter");
        LogCurrentState(L"dll_process_detach final state", false);
        StopLongRunningReportTimer(false);
        LogLifecycleEventWithoutLogFilePathInitialization(
            L"dll_process_detach_leave");
        break;
    }
    return TRUE;
}
