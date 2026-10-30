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

/* CopyFlags for FsGetFile / FsGetFileW */
#define FS_COPYFLAGS_OVERWRITE            1
#define FS_COPYFLAGS_RESUME               2
#define FS_COPYFLAGS_MOVE                 4
#define FS_COPYFLAGS_EXISTS_SAMECASE      8
#define FS_COPYFLAGS_EXISTS_DIFFERENTCASE 16

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

/* InfoStartEnd values passed to FsStatusInfo / FsStatusInfoW */
#define FS_STATUS_START 0
#define FS_STATUS_END   1

/* InfoOperation values passed to FsStatusInfo / FsStatusInfoW (values from the WFX SDK) */
#define FS_STATUS_OP_LIST              1
#define FS_STATUS_OP_GET_SINGLE        2
#define FS_STATUS_OP_GET_MULTI         3
#define FS_STATUS_OP_PUT_SINGLE        4
#define FS_STATUS_OP_PUT_MULTI         5
#define FS_STATUS_OP_RENMOV_SINGLE     6
#define FS_STATUS_OP_RENMOV_MULTI      7
#define FS_STATUS_OP_DELETE            8
#define FS_STATUS_OP_ATTRIB            9
#define FS_STATUS_OP_MKDIR            10
#define FS_STATUS_OP_EXEC             11
#define FS_STATUS_OP_CALCSIZE         12
#define FS_STATUS_OP_SEARCH           13
#define FS_STATUS_OP_SEARCH_TEXT      14
#define FS_STATUS_OP_SYNC_SEARCH      15
#define FS_STATUS_OP_SYNC_GET         16
#define FS_STATUS_OP_SYNC_PUT         17
#define FS_STATUS_OP_SYNC_DELETE      18
#define FS_STATUS_OP_GET_MULTI_THREAD 19
#define FS_STATUS_OP_PUT_MULTI_THREAD 20

/* Flags for tLogProc / tLogProcW */
#define MSGTYPE_CONNECT             1
#define MSGTYPE_DISCONNECT          2
#define MSGTYPE_DETAILS             3
#define MSGTYPE_TRANSFERCOMPLETE    4
#define MSGTYPE_CONNECTCOMPLETE     5
#define MSGTYPE_IMPORTANTERROR      6
#define MSGTYPE_OPERATIONCOMPLETE   7

/* Marks a FILETIME as "no date available" (Total Commander shows an empty column) */
#define FS_SET_TIME_UNKNOWN(ft) \
    do { (ft).dwHighDateTime = 0xFFFFFFFF; (ft).dwLowDateTime = 0xFFFFFFFE; } while (0)

/* Content field types returned by FsContentGetSupportedField / FsContentGetValue.
 * Values follow the Total Commander content plugin SDK (contentplugin.h); ft_nomorefields (0) ends
 * the enumeration. ft_delayed / ft_ondemand are deliberately NOT defined: this plugin never
 * returns them (every value comes from an in-memory cache) and their numeric values could not be
 * verified against the SDK headers, so an unused-but-wrong constant would be a trap. */
#define ft_nomorefields     0
#define ft_numeric_32       1
#define ft_numeric_64       2
#define ft_numeric_floating 3
#define ft_date             4
#define ft_time             5
#define ft_boolean          6
#define ft_multiplechoice   7
#define ft_string           8
#define ft_fulltext         9  /* Not supported in file system plugins */
#define ft_datetime        10
#define ft_stringw         11 /* "Should only be returned by Unicode function" (SDK header) */

/* Error values for FsContentGetValue */
#define ft_nosuchfield (-1)
#define ft_fileerror    (-2)
#define ft_fieldempty   (-3)

/* Flags passed to FsContentGetValue */
#define CONTENT_DELAYIFSLOW 1 /* TC would re-request a deliberately delayed field in a worker thread */

/* Unit indexes offered for the size-like fields (UnitIndex parameter of FsContentGetValue) */
#define CONTENT_UNIT_BYTES  0
#define CONTENT_UNIT_KBYTES 1
#define CONTENT_UNIT_MBYTES 2
#define CONTENT_UNIT_GBYTES 3

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

/* Remote file metadata passed by Total Commander to FsGetFile / FsGetFileW */
typedef struct {
    DWORD SizeLow;
    DWORD SizeHigh;
    FILETIME LastWriteTime;
    int Attr;
} RemoteInfoStruct;

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
void   __stdcall FsStatusInfo(char* RemoteDir, int InfoStartEnd, int InfoOperation);
void   __stdcall FsStatusInfoW(WCHAR* RemoteDir, int InfoStartEnd, int InfoOperation);
int    __stdcall FsGetFile(char* RemoteName, char* LocalName, int CopyFlags, RemoteInfoStruct* ri);
int    __stdcall FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct* ri);

/* Optional custom-columns surface (read-only: no FsContentSetValue).
 * The W variants are the ones Total Commander calls on NT-based systems (they only fall back to the
 * ANSI versions when the W exports are missing); FsContentGetSupportedField,
 * FsContentGetSupportedFieldFlags, FsContentGetDefaultSortOrder and FsContentPluginUnloading exist
 * in ANSI form only, as required by the SDK. */
int  __stdcall FsContentGetSupportedField(int FieldIndex, char* FieldName, char* Units, int maxlen);
int  __stdcall FsContentGetValue(char* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags);
int  __stdcall FsContentGetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags);
int  __stdcall FsContentGetSupportedFieldFlags(int FieldIndex);
int  __stdcall FsContentGetDefaultSortOrder(int FieldIndex);
BOOL __stdcall FsContentGetDefaultView(char* ViewContents, char* ViewHeaders, char* ViewWidths, char* ViewOptions, int maxlen);
BOOL __stdcall FsContentGetDefaultViewW(WCHAR* ViewContents, WCHAR* ViewHeaders, WCHAR* ViewWidths, WCHAR* ViewOptions, int maxlen);
void __stdcall FsContentStopGetValue(char* FileName);
void __stdcall FsContentStopGetValueW(WCHAR* FileName);
void __stdcall FsContentPluginUnloading(void);
