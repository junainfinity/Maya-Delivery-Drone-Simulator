#pragma once

#include "CoreMinimal.h"
#include "Containers/StringConv.h"

// Deliberately Windows-only. UE 5.3's generic archive drops OS flush errors
// and ignores FILEWRITE_NoReplaceExisting on this installed write path.
#if PLATFORM_WINDOWS && PLATFORM_64BITS && WITH_EDITOR
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"

namespace DDSN05Probe
{
    inline bool IsReportIOSupported() { return true; }

    inline bool ReportEndpointAbsent(const FString& Path)
    {
        const DWORD Attributes = ::GetFileAttributesW(*Path);
        if (Attributes != INVALID_FILE_ATTRIBUTES) return false;
        const DWORD Error = ::GetLastError();
        // Only a confirmed missing endpoint/path is accepted. Access errors
        // and unknown results fail closed before any registry or asset load.
        return Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND;
    }

    inline bool ReportDirectoryAncestorSafe(const FString& Path)
    {
        const DWORD Attributes = ::GetFileAttributesW(*Path);
        if (Attributes != INVALID_FILE_ATTRIBUTES)
            return (Attributes & FILE_ATTRIBUTE_DIRECTORY)!=0
                && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)==0;
        const DWORD Error = ::GetLastError();
        return Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND;
    }

    inline bool WriteExclusiveFlushedTemp(const FString& TempPath, const FString& Json)
    {
        // CREATE_NEW, not a GUID or UE file flag, provides exclusivity. No
        // sharing and no overlapped flag: every WriteFile call is synchronous.
        HANDLE Handle = ::CreateFileW(*TempPath,GENERIC_WRITE,0,nullptr,
            CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (Handle == INVALID_HANDLE_VALUE)
        {
            const DWORD Error = ::GetLastError();
            UE_LOG(LogTemp,Error,TEXT("N05 temp create failed path=%s win32=%lu; existing endpoint preserved"),
                *TempPath,Error);
            return false;
        }
        FTCHARToUTF8 Utf8(*Json);
        int32 Offset = 0;
        bool bWriteOkay = Utf8.Length()>0;
        while (bWriteOkay && Offset<Utf8.Length())
        {
            const DWORD Requested = static_cast<DWORD>(FMath::Min(1024*1024,Utf8.Length()-Offset));
            DWORD Written = 0;
            const bool bWritten = ::WriteFile(Handle,Utf8.Get()+Offset,Requested,&Written,nullptr)!=0;
            const DWORD Error = bWritten ? 0 : ::GetLastError();
            if (!bWritten || Written!=Requested)
            {
                UE_LOG(LogTemp,Error,TEXT("N05 temp write failed path=%s win32=%lu requested=%lu written=%lu"),
                    *TempPath,Error,Requested,Written);
                bWriteOkay=false;
                break;
            }
            Offset+=static_cast<int32>(Written);
        }
        bool bFlushOkay = false;
        if (bWriteOkay)
        {
            bFlushOkay = ::FlushFileBuffers(Handle)!=0;
            if (!bFlushOkay)
            {
                const DWORD Error = ::GetLastError();
                UE_LOG(LogTemp,Error,TEXT("N05 temp OS flush failed path=%s win32=%lu"),*TempPath,Error);
            }
        }
        // Close is attempted even after write/flush failure. Its result is
        // checked independently; any known failure prohibits publication.
        const bool bCloseOkay = ::CloseHandle(Handle)!=0;
        if (!bCloseOkay)
        {
            const DWORD Error = ::GetLastError();
            UE_LOG(LogTemp,Error,TEXT("N05 temp close failed path=%s win32=%lu"),*TempPath,Error);
        }
        // Failed created temps remain evidence; never delete a conflicting
        // endpoint. Successful finalization is handled by a no-replace move.
        return bWriteOkay && bFlushOkay && bCloseOkay;
    }
}

#include "Windows/HideWindowsPlatformTypes.h"
#else
namespace DDSN05Probe
{
    inline bool IsReportIOSupported() { return false; }
    inline bool ReportEndpointAbsent(const FString&) { return false; }
    inline bool ReportDirectoryAncestorSafe(const FString&) { return false; }
    inline bool WriteExclusiveFlushedTemp(const FString&, const FString&) { return false; }
}
#endif
