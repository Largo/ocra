/*
  Single Executable Bundle Stub
  This stub reads itself for embedded instructions to create directory
  and files in a temporary directory, launching a program.
*/

#include <windows.h>
#include <string.h>
#include <tchar.h>
#include <stdio.h>

const BYTE Signature[] = { 0x41, 0xb6, 0xba, 0x4e };

#define OP_END 0
#define OP_CREATE_DIRECTORY 1
#define OP_CREATE_FILE 2
#define OP_SETENV 5
#define OP_SET_SCRIPT 6
#define OP_MAX 7

BOOL ProcessImage(LPVOID p, DWORD size);
BOOL ProcessOpcodes(LPVOID* p);
void CreateAndWaitForProcess(LPTSTR ApplicationName, LPTSTR CommandLine);

BOOL OpEnd(LPVOID* p);
BOOL OpCreateFile(LPVOID* p);
BOOL OpCreateDirectory(LPVOID* p);
BOOL OpSetEnv(LPVOID* p);
BOOL OpSetScript(LPVOID* p);

#if WITH_LZMA
#include <LzmaDec.h>
BOOL DecompressLzma(LPVOID p, DWORD CompressedSize);
#endif

typedef BOOL (*POpcodeHandler)(LPVOID*);

LPTSTR Script_ApplicationName = NULL;
LPTSTR Script_CommandLine = NULL;

DWORD ExitStatus = 0;
BOOL ExitCondition = FALSE;
BOOL DebugModeEnabled = FALSE;
BOOL DeleteInstDirEnabled = FALSE;
BOOL ChdirBeforeRunEnabled = TRUE;
TCHAR ImageFileName[MAX_PATH];

#if _CONSOLE
#define FATAL(...) { fprintf(stderr, "FATAL ERROR: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#else
#define FATAL(...) { \
   TCHAR TextBuffer[1024]; \
   _sntprintf(TextBuffer, 1024, __VA_ARGS__); \
   MessageBox(NULL, TextBuffer, _T("OCRAN"), MB_OK | MB_ICONWARNING); \
   }
#endif

#if _CONSOLE
#define DEBUG(...) { if (DebugModeEnabled) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } }
#else
#define DEBUG(...)
#endif

POpcodeHandler OpcodeHandlers[OP_MAX] =
{
   &OpEnd,
   &OpCreateDirectory,
   &OpCreateFile,
   NULL,
   NULL,
   &OpSetEnv,
   &OpSetScript,
};

TCHAR InstDir[MAX_PATH];

/** Decoder: Zero-terminated string */
LPTSTR GetString(LPVOID* p)
{
    SIZE_T len = (SIZE_T)*(LPWORD)*p;
    *p += sizeof(WORD);
    LPTSTR str = *p;
    *p += len;
    return str;
}

/** Decoder: 32 bit unsigned integer */
DWORD GetInteger(LPVOID* p)
{
   DWORD dw = *(DWORD*)*p;
   *p += 4;
   return dw;
}

BYTE GetOpcode(LPVOID* p)
{
    BYTE op = *(LPBYTE)*p;
    *p += sizeof(BYTE);
    return op;
}

BOOL GetBool(LPVOID* p)
{
    BOOL b = (BOOL)*(LPBYTE)*p;
    *p += sizeof(BYTE);
    return b;
}

void GetHeader(LPVOID* p, LPBOOL debug_mode, LPBOOL debug_extract, LPBOOL delete_after, LPBOOL chdir_before, LPBOOL compressed)
{
    *debug_mode = GetBool(p);
    *debug_extract = GetBool(p);
    *delete_after = GetBool(p);
    *chdir_before = GetBool(p);
    *compressed = GetBool(p);
}

/**
   Handler for console events.
*/
BOOL WINAPI ConsoleHandleRoutine(DWORD dwCtrlType)
{
   // Ignore all events. They will also be dispatched to the child procress (Ruby) which should
   // exit quickly, allowing us to clean up.
   return TRUE;
}

void FindExeDir(TCHAR* d)
{
   strncpy(d, ImageFileName, MAX_PATH);
   unsigned int i;
   for (i = strlen(d)-1; i >= 0; --i)
   {
      if (i == 0 || d[i] == '\\')
      {
         d[i] = 0;
         break;
      }
   }
}

BOOL DeleteRecursively(LPTSTR path)
{
   TCHAR findPath[MAX_PATH];
   DWORD pathLength;
   WIN32_FIND_DATA findData;
   HANDLE handle;
   BOOL AnyFailed = FALSE;

   lstrcpy(findPath, path);
   pathLength = lstrlen(findPath);
   if (pathLength > 1 && pathLength < MAX_PATH - 2) {
      if (path[pathLength-1] == '\\')
         lstrcat(findPath, "*");
      else {
         lstrcat(findPath, "\\*");
         ++pathLength;
      }
      handle = FindFirstFile(findPath, &findData);
      findPath[pathLength] = 0;
      if (handle != INVALID_HANDLE_VALUE) {
         do {
            if (pathLength + lstrlen(findData.cFileName) < MAX_PATH) {
               TCHAR subPath[MAX_PATH];
               lstrcpy(subPath, findPath);
               lstrcat(subPath, findData.cFileName);
               if ((lstrcmp(findData.cFileName, ".") != 0) && (lstrcmp(findData.cFileName, "..") != 0)) {
                  if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                     if (!DeleteRecursively(subPath))
                        AnyFailed = TRUE;
                  } else {
                     if (!DeleteFile(subPath)) {
                        MoveFileEx(subPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                        AnyFailed = TRUE;
                     }
                  }
               }
            } else {
               AnyFailed = TRUE;
            }
         } while (FindNextFile(handle, &findData));
         FindClose(handle);
      }
   } else {
      AnyFailed = TRUE;
   }
   if (!RemoveDirectory(findPath)) {
      MoveFileEx(findPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
      AnyFailed = TRUE;
   }
   return AnyFailed;
}

void MarkForDeletion(LPTSTR path)
{
   TCHAR marker[MAX_PATH];
   lstrcpy(marker, path);
   lstrcat(marker, ".ocran-delete-me");
   HANDLE h = CreateFile(marker, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   CloseHandle(h);
}

BOOL CreateInstDirectory(BOOL DebugExtractMode)
{
   /* Create an installation directory that will hold the extracted files */
   TCHAR TempPath[MAX_PATH];
   if (DebugExtractMode)
   {
      // In debug extraction mode, create the temp directory next to the exe
      FindExeDir(TempPath);
      if (strlen(TempPath) == 0)
      {
         FATAL("Unable to find directory containing exe");
         return FALSE;
      }
   }
   else
   {
      GetTempPath(MAX_PATH, TempPath);
   }

   while (TRUE)
   {
      UINT tempResult = GetTempFileName(TempPath, _T("ocranstub"), 0, InstDir);
      if (tempResult == 0u)
      {
         FATAL("Failed to get temp file name.");
         return FALSE;
      }

      DEBUG("Creating installation directory: '%s'", InstDir);

      /* Attempt to delete the temp file created by GetTempFileName.
         Ignore errors, i.e. if it doesn't exist. */
      (void)DeleteFile(InstDir);

      if (CreateDirectory(InstDir, NULL))
      {
         break;
      }
      else if (GetLastError() != ERROR_ALREADY_EXISTS)
      {
         FATAL("Failed to create installation directory.");
         return FALSE;
      }
   }
   return TRUE;
}

int CALLBACK _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
   /* Find name of image */
   if (!GetModuleFileName(NULL, ImageFileName, MAX_PATH))
   {
      FATAL("Failed to get executable name (error %lu).", GetLastError());
      return -1;
   }


   /* By default, assume the installation directory is wherever the EXE is */
   FindExeDir(InstDir);

   /* Set up environment */
   SetEnvironmentVariable(_T("OCRAN_EXECUTABLE"), ImageFileName);

   SetConsoleCtrlHandler(&ConsoleHandleRoutine, TRUE);

   /* Open the image (executable) */
   HANDLE hImage = CreateFile(ImageFileName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
   if (hImage == INVALID_HANDLE_VALUE)
   {
      FATAL("Failed to open executable (%s)", ImageFileName);
      return -1;
   }

   /* Create a file mapping */
   DWORD FileSize = GetFileSize(hImage, NULL);
   HANDLE hMem = CreateFileMapping(hImage, NULL, PAGE_READONLY, 0, FileSize, NULL);
   if (hMem == INVALID_HANDLE_VALUE)
   {
      FATAL("Failed to create file mapping (error %lu)", GetLastError());
      CloseHandle(hImage);
      return -1;
   }

   /* Map the image into memory */
   LPVOID lpv = MapViewOfFile(hMem, FILE_MAP_READ, 0, 0, 0);
   if (lpv == NULL)
   {
      FATAL("Failed to map view of executable into memory (error %lu).", GetLastError());
   }
   else
   {
      if (!ProcessImage(lpv, FileSize))
      {
         ExitStatus = -1;
      }

      if (!UnmapViewOfFile(lpv))
      {
         FATAL("Failed to unmap view of executable.");
      }
   }

   if (!CloseHandle(hMem))
   {
      FATAL("Failed to close file mapping.");
   }

   if (!CloseHandle(hImage))
   {
      FATAL("Failed to close executable.");
   }

   if (ChdirBeforeRunEnabled)
   {
      DEBUG("Changing CWD to unpacked directory %s/src", InstDir);
      SetCurrentDirectory(InstDir);
      SetCurrentDirectory("./src");
   }

   if (Script_ApplicationName && Script_CommandLine)
   {
      DEBUG("**********");
      DEBUG("Starting app in: %s", InstDir);
      DEBUG("**********");
      CreateAndWaitForProcess(Script_ApplicationName, Script_CommandLine);
      LocalFree(Script_ApplicationName);
      LocalFree(Script_CommandLine);
      Script_ApplicationName = NULL;
      Script_CommandLine = NULL;
   }

   if (DeleteInstDirEnabled)
   {
      DEBUG("Deleting temporary installation directory %s", InstDir);
      TCHAR SystemDirectory[MAX_PATH];
      if (GetSystemDirectory(SystemDirectory, MAX_PATH) > 0)
         SetCurrentDirectory(SystemDirectory);
      else
         SetCurrentDirectory("C:\\");

      if (!DeleteRecursively(InstDir))
            MarkForDeletion(InstDir);
   }

   ExitProcess(ExitStatus);

   /* Never gets here */
   return 0;
}

/**
   Process the image by checking the signature and locating the first
   opcode.
*/
BOOL ProcessImage(LPVOID ptr, DWORD size)
{
    LPVOID pSig = ptr + size - sizeof(Signature);
    if (memcmp(pSig, Signature, sizeof(Signature)) != 0) {
        FATAL("Bad signature in executable.");
        return FALSE;
    }
    DEBUG("Good signature found.");

    LPVOID data_tail = pSig - sizeof(DWORD);
    DWORD OpcodeOffset = *(DWORD*)(data_tail);
    LPVOID pSeg = ptr + OpcodeOffset;

    BOOL debug_extract;
    BOOL compressed;
    GetHeader(&pSeg, &DebugModeEnabled, &debug_extract, &DeleteInstDirEnabled, &ChdirBeforeRunEnabled, &compressed);

    if (DebugModeEnabled)
        DEBUG("Ocran stub running in debug mode");

    CreateInstDirectory(debug_extract);

    if (compressed) {
#if WITH_LZMA
        DWORD data_len = data_tail - pSeg - 1; // 1 is OP_END
        if (!DecompressLzma(pSeg, data_len)) {
            return FALSE;
        }
        pSeg += data_len;
#else
        FATAL("Does not support LZMA");
        return FALSE;
#endif
    }

    return ProcessOpcodes(&pSeg);
}

/**
   Process the opcodes in memory.
*/
BOOL ProcessOpcodes(LPVOID* p)
{
   BOOL result = TRUE;

   while (result && !ExitCondition)
   {
      DWORD opcode = GetOpcode(p);
      if (opcode >= OP_MAX)
      {
         FATAL("Invalid opcode '%lu'.", opcode);
         return FALSE;
      }
      result = OpcodeHandlers[opcode](p);
   }

   return result;
}

/**
   Expands a specially formatted string, replacing | with the
   temporary installation directory.
*/
void ExpandPath(LPTSTR* out, LPTSTR str)
{
   DWORD OutSize = lstrlen(str) + sizeof(TCHAR);
   LPTSTR a = str;
   while ((a = _tcschr(a, L'|')))
   {
      OutSize += lstrlen(InstDir) - sizeof(TCHAR);
      a++;
   }

   *out = LocalAlloc(LMEM_FIXED, OutSize);

   LPTSTR OutPtr = *out;
   while ((a = _tcschr(str, L'|')))
   {
      int l = a - str;
      if (l > 0)
      {
         memcpy(OutPtr, str, l);
         OutPtr += l;
         str += l;
      }
      str += sizeof(TCHAR);
      lstrcpy(OutPtr, InstDir);
      OutPtr += lstrlen(OutPtr);
   }
   lstrcpy(OutPtr, str);
}

/**
   Finds the start of the first argument after the current one. Handles quoted arguments.
*/
LPTSTR SkipArg(LPTSTR str)
{
   if (*str == '"')
   {
      str++;
      while (*str && *str != '"') { str++; }
      if (*str == '"') { str++; }
   }
   else
   {
      while (*str && *str != ' ') { str++; }
   }
   while (*str && *str != ' ') { str++; }
   return str;
}

/**
   Create a file (OP_CREATE_FILE opcode handler)
*/
BOOL OpCreateFile(LPVOID* p)
{
   BOOL Result = TRUE;
   LPTSTR FileName = GetString(p);
   DWORD FileSize = GetInteger(p);
   LPVOID Data = *p;
   *p += FileSize;

   TCHAR Fn[MAX_PATH];
   lstrcpy(Fn, InstDir);
   lstrcat(Fn, _T("\\"));
   lstrcat(Fn, FileName);

   DEBUG("CreateFile(%s, %lu)", Fn, FileSize);
   HANDLE hFile = CreateFile(Fn, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
   if (hFile != INVALID_HANDLE_VALUE)
   {
      DWORD BytesWritten;
      if (!WriteFile(hFile, Data, FileSize, &BytesWritten, NULL))
      {
         FATAL("Write failure (%lu)", GetLastError());
         Result = FALSE;
      }
      if (BytesWritten != FileSize)
      {
         FATAL("Write size failure");
         Result = FALSE;
      }
      CloseHandle(hFile);
   }
   else
   {
      FATAL("Failed to create file '%s'", Fn);
      Result = FALSE;
   }

   return Result;
}

/**
   Create a directory (OP_CREATE_DIRECTORY opcode handler)
*/
BOOL OpCreateDirectory(LPVOID* p)
{
   LPTSTR DirectoryName = GetString(p);

   TCHAR DirName[MAX_PATH];
   lstrcpy(DirName, InstDir);
   lstrcat(DirName, _T("\\"));
   lstrcat(DirName, DirectoryName);

   DEBUG("CreateDirectory(%s)", DirName);

   if (!CreateDirectory(DirName, NULL))
   {
      if (GetLastError() == ERROR_ALREADY_EXISTS)
      {
         DEBUG("Directory already exists");
      }
      else
      {
         FATAL("Failed to create directory '%s'.", DirName);
         return FALSE;
      }
   }

   return TRUE;
}

void GetScriptInfo(LPTSTR ImageName, LPTSTR* pApplicationName, LPTSTR CmdLine, LPTSTR* pCommandLine)
{
   ExpandPath(pApplicationName, ImageName);

   LPTSTR ExpandedCommandLine;
   ExpandPath(&ExpandedCommandLine, CmdLine);

   LPTSTR MyCmdLine = GetCommandLine();
   LPTSTR MyArgs = SkipArg(MyCmdLine);

   *pCommandLine = LocalAlloc(LMEM_FIXED, lstrlen(ExpandedCommandLine) + sizeof(TCHAR) + lstrlen(MyArgs) + sizeof(TCHAR));
   lstrcpy(*pCommandLine, ExpandedCommandLine);
   lstrcat(*pCommandLine, _T(" "));
   lstrcat(*pCommandLine, MyArgs);

   LocalFree(ExpandedCommandLine);
}

void CreateAndWaitForProcess(LPTSTR ApplicationName, LPTSTR CommandLine)
{
   PROCESS_INFORMATION ProcessInformation;
   STARTUPINFO StartupInfo;
   ZeroMemory(&StartupInfo, sizeof(StartupInfo));
   StartupInfo.cb = sizeof(StartupInfo);
   BOOL r = CreateProcess(ApplicationName, CommandLine, NULL, NULL,
                          TRUE, 0, NULL, NULL, &StartupInfo, &ProcessInformation);

   if (!r)
   {
      FATAL("Failed to create process (%s): %lu", ApplicationName, GetLastError());
      return;
   }

   WaitForSingleObject(ProcessInformation.hProcess, INFINITE);

   if (!GetExitCodeProcess(ProcessInformation.hProcess, &ExitStatus))
   {
      FATAL("Failed to get exit status (error %lu).", GetLastError());
   }

   CloseHandle(ProcessInformation.hProcess);
   CloseHandle(ProcessInformation.hThread);
}

/**
 * Sets up a process to be created after all other opcodes have been processed. This can be used to create processes
 * after the temporary files have all been created and memory has been freed.
 */
BOOL OpSetScript(LPVOID* p)
{
    DEBUG("SetScript");
    if (Script_ApplicationName || Script_CommandLine) {
        FATAL("Script is already set")
        return FALSE;
    }

    LPTSTR app_name = GetString(p);
    LPTSTR cmd_line = GetString(p);

    GetScriptInfo(app_name, &Script_ApplicationName, cmd_line, &Script_CommandLine);

    return TRUE;
}

#if WITH_LZMA
void* SzAlloc(void* p, size_t size) { p = p; return LocalAlloc(LMEM_FIXED, size); }
void SzFree(void* p, void* address) { p = p; LocalFree(address); }
ISzAlloc alloc = { SzAlloc, SzFree };

#define LZMA_UNPACKSIZE_SIZE 8
#define LZMA_HEADER_SIZE (LZMA_PROPS_SIZE + LZMA_UNPACKSIZE_SIZE)

BOOL DecompressLzma(LPVOID p, DWORD CompressedSize)
{
   BOOL Success = TRUE;

   DEBUG("LzmaDecode(%ld)", CompressedSize);

   Byte* src = (Byte*)p;

   UInt64 unpackSize = 0;
   int i;
   for (i = 0; i < 8; i++)
   {
      unpackSize += (UInt64)src[LZMA_PROPS_SIZE + i] << (i * 8);
   }

   Byte* DecompressedData = LocalAlloc(LMEM_FIXED, unpackSize);

   SizeT lzmaDecompressedSize = unpackSize;
   SizeT inSizePure = CompressedSize - LZMA_HEADER_SIZE;
   ELzmaStatus status;
   SRes res = LzmaDecode(DecompressedData, &lzmaDecompressedSize, src + LZMA_HEADER_SIZE, &inSizePure,
                         src, LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &status, &alloc);
   if (res != SZ_OK)
   {
      FATAL("LZMA decompression failed.");
      Success = FALSE;
   }
   else
   {
      LPVOID decPtr = DecompressedData;
      if (!ProcessOpcodes(&decPtr))
      {
         Success = FALSE;
      }
   }

   LocalFree(DecompressedData);
   return Success;
}
#endif

BOOL OpEnd(LPVOID* p)
{
   ExitCondition = TRUE;
   return TRUE;
}

BOOL OpSetEnv(LPVOID* p)
{
   LPTSTR Name = GetString(p);
   LPTSTR Value = GetString(p);
   LPTSTR ExpandedValue;
   ExpandPath(&ExpandedValue, Value);
   DEBUG("SetEnv(%s, %s)", Name, ExpandedValue);

   BOOL Result = FALSE;
   if (!SetEnvironmentVariable(Name, ExpandedValue))
   {
      FATAL("Failed to set environment variable (error %lu).", GetLastError());
      Result = FALSE;
   }
   else
   {
      Result = TRUE;
   }
   LocalFree(ExpandedValue);
   return Result;
}
