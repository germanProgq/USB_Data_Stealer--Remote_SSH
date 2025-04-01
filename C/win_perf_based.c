/*
 * Windows-Only High-Performance Backup Program in C
 *
 * Key Features:
 *  - Checks "admin" privileges.
 *  - Lists Windows drives (DRIVE_FIXED or DRIVE_REMOVABLE).
 *  - Lets the user choose which drive to back up.
 *  - Stores volume ID in "backup_<drive_letter>\\volume_id.txt" so it can detect repeated inserts.
 *  - Skips certain folders by name.
 *  - Copies only if source is newer or destination is missing.
 *  - Uses multi-threaded file copying for high throughput.
 *  - Displays progress and final summary.
 *
 * Compile (Windows example):
 *   cl /O2 backup_windows.c
 */

 
 #include <windows.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <ctype.h>
 #include <direct.h>
 #include <io.h>
 #include <sys/types.h>
 #include <sys/stat.h>

#ifndef COPY_FILE_NO_BUFFERING
#define COPY_FILE_NO_BUFFERING 0x00001000
#endif

 
 // Link advapi32 for checking admin privileges (if needed).
 #pragma comment(lib, "Advapi32.lib")
 
 #ifndef PATH_MAX
   #define PATH_MAX 260
 #endif
 
 #define ACCESS       _access
 #define MKDIR(a)     _mkdir(a)
 #define STAT         _stat
 typedef struct _stat stat_t;
 
 // ---------------------------------------------------------------------
 // Colored console output (optional but kept for clarity)
 // ---------------------------------------------------------------------
 static void SetColorError(void)
 {
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // Bright red
         SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_INTENSITY);
     }
 }
 
 static void SetColorWarning(void)
 {
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // Yellow: red + green
         SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
     }
 }
 
 static void SetColorInfo(void)
 {
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // Cyan: green + blue
         SetConsoleTextAttribute(h, FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
     }
 }
 
 static void SetColorSuccess(void)
 {
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // Bright green
         SetConsoleTextAttribute(h, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
     }
 }
 
 static void ResetColor(void)
 {
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // typical default: bright white on black
         SetConsoleTextAttribute(
             h, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
     }
 }
 
 static void PrintError(const char *msg)
 {
     SetColorError();
     fprintf(stderr, "[ERROR] %s\n", msg);
     ResetColor();
 }
 
 static void PrintWarning(const char *msg)
 {
     SetColorWarning();
     fprintf(stderr, "[WARNING] %s\n", msg);
     ResetColor();
 }
 
 static void PrintInfo(const char *msg)
 {
     SetColorInfo();
     printf("[INFO] %s\n", msg);
     ResetColor();
 }
 
 static void PrintSuccess(const char *msg)
 {
     SetColorSuccess();
     printf("[SUCCESS] %s\n", msg);
     ResetColor();
 }
 
 // ---------------------------------------------------------------------
 // Check if running as Administrator
 // ---------------------------------------------------------------------
 static int IsRunningAsAdmin(void)
 {
     // A quick test: try writing to %SystemRoot%\Temp\test_admin.txt
     char systemRoot[MAX_PATH];
     DWORD len = GetEnvironmentVariableA("SystemRoot", systemRoot, MAX_PATH);
     if (len == 0 || len > MAX_PATH - 1) {
         strcpy(systemRoot, "C:\\Windows");
     }
     char testFile[MAX_PATH];
     _snprintf(testFile, MAX_PATH, "%s\\Temp\\test_admin.txt", systemRoot);
 
     HANDLE hFile = CreateFileA(testFile, GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
     if (hFile == INVALID_HANDLE_VALUE) {
         return 0; // fail => probably not admin
     }
     CloseHandle(hFile);
     DeleteFileA(testFile);
     return 1; // likely admin
 }
 
 // ---------------------------------------------------------------------
 // Skip Folders
 // ---------------------------------------------------------------------
 static const char* SKIP_FOLDERS[] = {
     "$Recycle.Bin",
     "System Volume Information",
     "Windows",
     "Program Files",
     "Program Files (x86)",
     "ProgramData",
     ".DS_Store",
     ".Spotlight-V100",
     ".Trashes"
 };
 static const int NUM_SKIP_FOLDERS =
     (int)(sizeof(SKIP_FOLDERS)/sizeof(SKIP_FOLDERS[0]));
 
 static int ShouldSkipFolder(const char *folderName)
 {
     if (!folderName || !*folderName) return 0;
     for (int i = 0; i < NUM_SKIP_FOLDERS; i++) {
         if (_stricmp(folderName, SKIP_FOLDERS[i]) == 0) {
             return 1;
         }
     }
     return 0;
 }
 
 // ---------------------------------------------------------------------
 // List Windows drives (DRIVE_FIXED or DRIVE_REMOVABLE)
 // ---------------------------------------------------------------------
 static int ListDrives(char drivesList[][4], int maxCount)
 {
     char buffer[512];
     DWORD len = GetLogicalDriveStringsA((DWORD)sizeof(buffer), buffer);
     if (len == 0 || len > sizeof(buffer)) {
         return 0;
     }
     int count = 0;
     char *p = buffer;
     while (*p && count < maxCount) {
         UINT dt = GetDriveTypeA(p);
         if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE) {
             // e.g. "C:\"
             strncpy(drivesList[count], p, 3);
             drivesList[count][3] = '\0';
             count++;
         }
         p += strlen(p) + 1;
     }
     return count;
 }
 
 // ---------------------------------------------------------------------
 // Volume ID
 // ---------------------------------------------------------------------
 static unsigned long GetVolumeID(const char *drivePath)
 {
     // drivePath like "E:\" or "C:\"
     DWORD serial = 0;
     GetVolumeInformationA(drivePath, NULL, 0, &serial, NULL, NULL, NULL, 0);
     return (unsigned long)serial;
 }
 
 // Write/read volume ID from e.g. backup_C\volume_id.txt
 static int WriteVolumeID(const char *folder, unsigned long id)
 {
     char path[PATH_MAX];
     _snprintf(path, PATH_MAX, "%s\\volume_id.txt", folder);
 
     FILE *f = fopen(path, "w");
     if (!f) return 0;
     fprintf(f, "%lu", id);
     fclose(f);
     return 1;
 }
 
 static unsigned long ReadVolumeID(const char *folder)
 {
     char path[PATH_MAX];
     _snprintf(path, PATH_MAX, "%s\\volume_id.txt", folder);
 
     FILE *f = fopen(path, "r");
     if (!f) return 0;
     unsigned long val = 0;
     fscanf(f, "%lu", &val);
     fclose(f);
     return val;
 }
 
 // ---------------------------------------------------------------------
 // Check if source is newer than destination
 // ---------------------------------------------------------------------
 static int SourceIsNewer(const char *src, const char *dst)
 {
     stat_t srcStat, dstStat;
     if (STAT(src, &srcStat) != 0) {
         // can't read src => skip
         return 0;
     }
     if (STAT(dst, &dstStat) != 0) {
         // dest doesn't exist => definitely copy
         return 1;
     }
     // Compare mod times
     return (srcStat.st_mtime > dstStat.st_mtime) ? 1 : 0;
 }
 
 // ---------------------------------------------------------------------
 // We will store a list of (src, dst) pairs to copy
 // ---------------------------------------------------------------------
 typedef struct {
     char src[PATH_MAX];
     char dst[PATH_MAX];
 } FilePair;
 
 static FilePair *gFileArray = NULL;
 static int       gFileCount = 0;
 static int       gFileCap   = 0;
 
 static void EnsureFileArrayCapacity(void)
 {
     if (gFileCount >= gFileCap) {
         int newCap = (gFileCap == 0) ? 1024 : gFileCap * 2;
         FilePair *newArr = (FilePair*)malloc(newCap * sizeof(FilePair));
         if (gFileArray) {
             memcpy(newArr, gFileArray, gFileCount * sizeof(FilePair));
             free(gFileArray);
         }
         gFileArray = newArr;
         gFileCap   = newCap;
     }
 }
 
 static void AddFilePair(const char *src, const char *dst)
 {
     EnsureFileArrayCapacity();
     strncpy(gFileArray[gFileCount].src, src, PATH_MAX - 1);
     gFileArray[gFileCount].src[PATH_MAX - 1] = '\0';
 
     strncpy(gFileArray[gFileCount].dst, dst, PATH_MAX - 1);
     gFileArray[gFileCount].dst[PATH_MAX - 1] = '\0';
 
     gFileCount++;
 }
 
 // ---------------------------------------------------------------------
 // Create parent directories if needed
 // ---------------------------------------------------------------------
 static void CreateParentDir(const char *dst)
 {
     // Find last backslash
     char tmp[PATH_MAX];
     strncpy(tmp, dst, PATH_MAX - 1);
     tmp[PATH_MAX - 1] = '\0';
 
     char *p = strrchr(tmp, '\\');
     if (!p) {
         // no directory part
         return;
     }
     *p = '\0'; // isolate the directory path
 
     // We do a simplistic approach: try to mkdir the entire path
     // segment by segment
     char *sep = tmp;
     while (1) {
         char *slashPos = strchr(sep, '\\');
         if (!slashPos) {
             // last segment
             if (*tmp) {
                 MKDIR(tmp); // ignore failure if it exists
             }
             break;
         }
         *slashPos = '\0';
         if (*tmp) {
             MKDIR(tmp);
         }
         *slashPos = '\\';
         sep = slashPos + 1;
     }
 }
 
 // ---------------------------------------------------------------------
 // Copy using CopyFileEx for potentially better performance
 // ---------------------------------------------------------------------
 static DWORD WINAPI CopyProgressCallback(
     LARGE_INTEGER TotalFileSize,
     LARGE_INTEGER TotalBytesTransferred,
     LARGE_INTEGER StreamSize,
     LARGE_INTEGER StreamBytesTransferred,
     DWORD dwStreamNumber,
     DWORD dwCallbackReason,
     HANDLE hSourceFile,
     HANDLE hDestinationFile,
     LPVOID lpData)
 {
     // We won't do per-file progress here; just continue
     return PROGRESS_CONTINUE;
 }
 
 static int CopyOneFile(const char *src, const char *dst)
 {
     CreateParentDir(dst);
 
     // Use wide-char conversions for CopyFileExW if you'd like, but here
     // we'll just use CopyFileExA to keep it straightforward:
     BOOL res = CopyFileExA(
         src, dst,                // source, destination
         (LPPROGRESS_ROUTINE)CopyProgressCallback,
         NULL,                    // lpData
         NULL,                    // pbCancel
         COPY_FILE_NO_BUFFERING   // for large I/O sometimes helps performance
     );
 
     return res ? 1 : 0;
 }
 
 // ---------------------------------------------------------------------
 // Recursively enumerate source => build (src, dst) list
 // Use FindFirstFileExA with FindExInfoBasic to reduce overhead
 // ---------------------------------------------------------------------
 static void RecurseEnumerate(const char *srcDir, const char *dstDir)
 {
     char searchPath[PATH_MAX];
     _snprintf(searchPath, PATH_MAX, "%s\\*", srcDir);
 
     WIN32_FIND_DATAA fdata;
     HANDLE hFind = FindFirstFileExA(
         searchPath,
         FindExInfoBasic,   // less overhead than FindExInfoStandard
         &fdata,
         FindExSearchNameMatch,
         NULL,
         0
     );
 
     if (hFind == INVALID_HANDLE_VALUE) {
         return;
     }
     do {
         const char *name = fdata.cFileName;
         if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
             continue;
         }
 
         char fullSrc[PATH_MAX];
         _snprintf(fullSrc, PATH_MAX, "%s\\%s", srcDir, name);
 
         // If directory
         if (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
             if (!ShouldSkipFolder(name)) {
                 char fullDst[PATH_MAX];
                 _snprintf(fullDst, PATH_MAX, "%s\\%s", dstDir, name);
                 RecurseEnumerate(fullSrc, fullDst);
             }
         } else {
             // It's a file
             char fullDst[PATH_MAX];
             _snprintf(fullDst, PATH_MAX, "%s\\%s", dstDir, name);
             AddFilePair(fullSrc, fullDst);
         }
 
     } while (FindNextFileA(hFind, &fdata) != 0);
 
     FindClose(hFind);
 }
 
 // ---------------------------------------------------------------------
 // Multi-threading for file copies
 // ---------------------------------------------------------------------
 static volatile LONG gNextIndex   = 0;
 static volatile LONG gFilesCopied = 0;
 static volatile LONG gFilesFailed = 0;
 static volatile LONG gFilesSkipped= 0;
 static volatile LONG gFilesDone   = 0;
 static volatile LONG gFilesTotal  = 0;
 
 DWORD WINAPI WorkerThread(LPVOID param)
 {
     (void)param; // unused
     while (1) {
         LONG idx = InterlockedIncrement(&gNextIndex) - 1;
         if (idx >= gFilesTotal) {
             break;
         }
         const char *src = gFileArray[idx].src;
         const char *dst = gFileArray[idx].dst;
 
         if (SourceIsNewer(src, dst)) {
             if (CopyOneFile(src, dst)) {
                 InterlockedIncrement(&gFilesCopied);
             } else {
                 InterlockedIncrement(&gFilesFailed);
             }
         } else {
             InterlockedIncrement(&gFilesSkipped);
         }
         InterlockedIncrement(&gFilesDone);
     }
     return 0;
 }
 
 // ---------------------------------------------------------------------
 // MAIN
 // ---------------------------------------------------------------------
 int main(void)
 {
     // 1) Check admin
     if (!IsRunningAsAdmin()) {
         PrintError("Please run this program as Administrator (right-click -> Run as administrator).");
         printf("Press ENTER to exit...\n");
         getchar();
         return 1;
     }
 
     // 2) List drives
     char drivesList[26][4]; // up to 26
     int driveCount = ListDrives(drivesList, 26);
 
     if (driveCount == 0) {
         PrintError("No drives/volumes detected.");
         printf("Press ENTER to exit...\n");
         getchar();
         return 1;
     }
 
     PrintInfo("Detected drives:");
     for (int i = 0; i < driveCount; i++) {
         printf("  [%d] %s\n", i + 1, drivesList[i]);
     }
 
     printf("Which drive would you like to back up? (1 - %d): ", driveCount);
     int choice = 0;
     if (scanf("%d", &choice) != 1 || choice < 1 || choice > driveCount) {
         PrintError("Invalid choice. Aborting.");
         getchar(); getchar();
         return 1;
     }
 
     // Chosen
     char chosenDrive[4];
     strcpy(chosenDrive, drivesList[choice - 1]); // e.g. "E:\"
 
     // Confirm
     printf("You chose: %s. Continue? (Y/N): ", chosenDrive);
     char c;
     scanf(" %c", &c);
     if (tolower(c) != 'y') {
         PrintWarning("Backup canceled by user.");
         getchar(); getchar();
         return 0;
     }
 
     // 3) Build backup folder "backup_<driveLetter>"
     char backupFolder[PATH_MAX];
     char letter = toupper(chosenDrive[0]);
 
     char currentDir[PATH_MAX];
     GetCurrentDirectoryA(PATH_MAX, currentDir);
     _snprintf(backupFolder, PATH_MAX, "%s\\backup_%c", currentDir, letter);
 
     if (MKDIR(backupFolder) != 0) {
         if (errno != EEXIST) {
             PrintError("Could not create backup folder.");
             getchar(); getchar();
             return 1;
         }
     }
 
     // 4) Check volume ID
     unsigned long volID = GetVolumeID(chosenDrive);
     unsigned long existingID = ReadVolumeID(backupFolder);
     if (existingID == 0) {
         // first time
         if (!WriteVolumeID(backupFolder, volID)) {
             PrintWarning("Could not write volume_id.txt. We'll continue anyway.");
         }
     } else {
         if (existingID != volID) {
             PrintWarning("The backup folder was used for a different volume previously!");
             printf("Proceed anyway? (Y/N): ");
             char ans = 0;
             scanf(" %c", &ans);
             if (tolower(ans) != 'y') {
                 PrintWarning("Aborted by user.");
                 getchar(); getchar();
                 return 0;
             }
         }
     }
 
     // 5) Enumerate files
     PrintInfo("Enumerating files...");
     gFileCount = 0;
     gFileCap   = 0;
     if (gFileArray) {
         free(gFileArray);
         gFileArray = NULL;
     }
 
     RecurseEnumerate(chosenDrive, backupFolder);
     if (gFileCount == 0) {
         PrintWarning("No files found or no access.");
         printf("Press ENTER to exit.\n");
         getchar(); getchar();
         return 0;
     }
     printf("Found %d files.\n", gFileCount);
 
     // 6) Multi-thread copy
     gNextIndex   = 0;
     gFilesCopied = 0;
     gFilesFailed = 0;
     gFilesSkipped= 0;
     gFilesDone   = 0;
     gFilesTotal  = gFileCount;
 
     SYSTEM_INFO si;
     GetSystemInfo(&si);
     int numCores = si.dwNumberOfProcessors;
     // Let’s allow up to 64 threads:
     if (numCores < 1)  numCores = 1;
     if (numCores > 64) numCores = 64;
 
     printf("Using %d threads...\n", numCores);
 
     HANDLE *threads = (HANDLE*)malloc(numCores * sizeof(HANDLE));
     for (int i = 0; i < numCores; i++) {
         threads[i] = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
     }
 
     // Main thread: show progress
     while (1) {
         LONG done = gFilesDone;
         int percent = (int)((done * 100) / gFilesTotal);
         printf("\rProgress: %3d%% (%ld/%ld)", percent, (long)done, (long)gFilesTotal);
         fflush(stdout);
         if (done >= (LONG)gFilesTotal) {
             break;
         }
         Sleep(250);
     }
     printf("\n");
 
     WaitForMultipleObjects(numCores, threads, TRUE, INFINITE);
     for (int i = 0; i < numCores; i++) {
         CloseHandle(threads[i]);
     }
     free(threads);
 
     long copied  = gFilesCopied;
     long failed  = gFilesFailed;
     long skipped = gFilesSkipped;
 
     // Summary
     if (failed == 0) {
         if (copied > 0) {
             char msg[256];
             _snprintf(msg, sizeof(msg),
                       "Backup complete! Copied %ld files; %ld were already up-to-date.",
                       copied, skipped);
             PrintSuccess(msg);
         } else {
             PrintInfo("All files were already up-to-date. Nothing needed copying.");
         }
     } else {
         char msg[256];
         _snprintf(msg, sizeof(msg),
                   "Backup finished with errors. Copied:%ld Failed:%ld Skipped:%ld",
                   copied, failed, skipped);
         PrintWarning(msg);
     }
 
     printf("\nPress ENTER to exit...\n");
     getchar(); getchar();
 
     free(gFileArray);
     gFileArray = NULL;
     return 0;
 }
 