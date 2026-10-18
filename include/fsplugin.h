#pragma once
#include <windows.h>

/* Return codes for FsGetFile / FsPutFile / FsRenMovFile */
#define FS_FILE_OK                  0
#define FS_FILE_EXISTS              1
#define FS_FILE_NOTFOUND            2
#define FS_FILE_READERROR           3
#define FS_FILE_WRITEERROR          4
#define FS_FILE_USERABORT           5
#define FS_FILE_NOTSUPPORTED        6
#define FS_FILE_EXISTSRESUMEALLOWED 7

/* Return codes for FsExecuteFile */
#define FS_EXEC_OK                  0
#define FS_EXEC_ERROR               1
#define FS_EXEC_YOURSELF           -1
#define FS_EXEC_SYMLINK            -2

/* Flags for tRequestProc / tRequestProcW */
#define RT_Other                    0
#define RT_UserName                 1
#define RT_Password                 2
#define RT_Account                  3
#define RT_UserNameFirewall         4
#define RT_PasswordFirewall         5
#define RT_TargetDir                6
#define RT_URL                      7
#define RT_MsgOK                    8
#define RT_MsgYesNo                 9
#define RT_MsgOKCancel              10

/* Flags for tLogProc / tLogProcW */
#define MSGTYPE_CONNECT             1
#define MSGTYPE_DISCONNECT          2
#define MSGTYPE_DETAILS             3
#define MSGTYPE_TRANSFERCOMPLETE    4
#define MSGTYPE_CONNECTCOMPLETE     5
#define MSGTYPE_IMPORTANTERROR      6
#define MSGTYPE_OPERATIONCOMPLETE   7

/* ANSI and Unicode host callback function types */
typedef int  (__stdcall *tProgressProc)(int PluginNr, char* SourceName, char* TargetName, int PercentDone);
typedef int  (__stdcall *tProgressProcW)(int PluginNr, WCHAR* SourceName, WCHAR* TargetName, int PercentDone);
typedef void (__stdcall *tLogProc)(int PluginNr, int MsgType, char* LogString);
typedef void (__stdcall *tLogProcW)(int PluginNr, int MsgType, WCHAR* LogString);
typedef BOOL (__stdcall *tRequestProc)(int PluginNr, int RequestType, char* CustomTitle, char* CustomText, char* ReturnedText, int maxlen);
typedef BOOL (__stdcall *tRequestProcW)(int PluginNr, int RequestType, WCHAR* CustomTitle, WCHAR* CustomText, WCHAR* ReturnedText, int maxlen);

/* Default parameters passed by Total Commander in FsSetDefaultParams */
typedef struct {
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char DefaultIniName[MAX_PATH];
} FsDefaultParamStruct;

/* Exported WFX plugin entry points (Unicode and mandatory ANSI stubs) */
int    __stdcall FsInit(int PluginNr, tProgressProc pProgressProc, tLogProc pLogProc, tRequestProc pRequestProc);
int    __stdcall FsInitW(int PluginNr, tProgressProcW pProgressProcW, tLogProcW pLogProcW, tRequestProcW pRequestProcW);
void   __stdcall FsGetDefRootName(char* DefRootName, int maxlen);
void   __stdcall FsSetDefaultParams(FsDefaultParamStruct* dps);
HANDLE __stdcall FsFindFirst(char* Path, WIN32_FIND_DATAA* FindData);
HANDLE __stdcall FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData);
BOOL   __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA* FindData);
BOOL   __stdcall FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData);
int    __stdcall FsFindClose(HANDLE Hdl);
