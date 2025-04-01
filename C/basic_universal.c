/*
 * Cross-Platform (Windows / macOS) Backup Program in C
 * 
 * - Checks "admin" (root on macOS, or admin on Windows).
 * - Lists drives: On Windows, uses GetLogicalDriveStrings. On macOS, enumerates /Volumes.
 * - Lets user choose which drive/volume to back up.
 * - Stores volume ID in backup_<volume_name>/volume_id.txt so it can detect repeated inserts of the same drive.
 * - Skips certain folders (customizable).
 * - Copies only if newer or missing.
 * - Uses multithreading for faster copies.
 * - Displays progress and final summary.
 *
 * NOTE: This is example-level code for demonstration.
 *       Compile with something like:
 *         Windows: cl /Zi /MD /O2 backup_cross.c
 *         macOS:   cc -O2 backup_cross.c -o backup_cross -lpthread
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <ctype.h>
 #include <sys/stat.h>
 #include <errno.h>
 
 // Platform detection
 #if defined(_WIN32) || defined(_WIN64)
   #define IS_WINDOWS 1
 #else
   #define IS_WINDOWS 0
 #endif
 
 #if IS_WINDOWS
   // -------------------- Windows Headers --------------------
   #include <windows.h>
   #include <direct.h>  // _mkdir
   #include <io.h>      // _access
 
   #pragma comment(lib, "Advapi32.lib")
 
   #ifndef PATH_MAX
     #define PATH_MAX 260
   #endif
 
   #define ACCESS  _access
   #define MKDIR(a) _mkdir(a)
 
   // For stat on Windows
   #include <sys/types.h>
   #include <sys/stat.h>
   #define STAT _stat
   typedef struct _stat stat_t;
 
 #else
   // -------------------- macOS (POSIX) --------------------
   #include <unistd.h>       // getuid, access, etc.
   #include <dirent.h>       // opendir, readdir
   #include <pthread.h>
   #include <fcntl.h>
   #include <sys/statvfs.h>
   #include <sys/mount.h>    // statfs
   #include <sys/param.h>
 
   #ifndef PATH_MAX
     #define PATH_MAX 1024
   #endif
 
   #define ACCESS  access
   #define MKDIR(a) mkdir((a), 0777)
 
   #define STAT stat
   typedef struct stat stat_t;
 
 #endif
 
 //---------------------------------------------------------------------
 // Colors
 //---------------------------------------------------------------------
 static void SetColorError(void)
 {
 #if IS_WINDOWS
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // bright red
         SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_INTENSITY);
     }
 #else
     // ANSI escape for bright red
     fprintf(stderr, "\033[1;31m");
 #endif
 }
 
 static void SetColorWarning(void)
 {
 #if IS_WINDOWS
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // yellow: red + green
         SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
     }
 #else
     // ANSI escape for yellow
     fprintf(stderr, "\033[1;33m");
 #endif
 }
 
 static void SetColorInfo(void)
 {
 #if IS_WINDOWS
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // cyan: green + blue
         SetConsoleTextAttribute(h, FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
     }
 #else
     // ANSI escape for cyan
     fprintf(stdout, "\033[1;36m");
 #endif
 }
 
 static void SetColorSuccess(void)
 {
 #if IS_WINDOWS
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // bright green
         SetConsoleTextAttribute(h, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
     }
 #else
     // ANSI escape for bright green
     fprintf(stdout, "\033[1;32m");
 #endif
 }
 
 static void ResetColor(void)
 {
 #if IS_WINDOWS
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         // typical default: bright white on black
         SetConsoleTextAttribute(h, 
             FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
     }
 #else
     // reset ANSI color
     fprintf(stdout, "\033[0m");
 #endif
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
 
 //---------------------------------------------------------------------
 // Check Admin (Windows) or root (macOS)
 //---------------------------------------------------------------------
 static int IsRunningAsAdmin(void)
 {
 #if IS_WINDOWS
     // Same trivial test: attempt writing to %SystemRoot%\Temp\test_admin.txt
     char systemRoot[MAX_PATH];
     DWORD len = GetEnvironmentVariableA("SystemRoot", systemRoot, MAX_PATH);
     if (len == 0 || len > MAX_PATH - 1) {
         strcpy(systemRoot, "C:\\Windows");
     }
     char testFile[MAX_PATH];
     _snprintf(testFile, MAX_PATH, "%s\\Temp\\test_admin.txt", systemRoot);
 
     HANDLE hFile = CreateFileA(
         testFile,
         GENERIC_WRITE,
         0,
         NULL,
         CREATE_ALWAYS,
         FILE_ATTRIBUTE_NORMAL,
         NULL
     );
     if (hFile == INVALID_HANDLE_VALUE) {
         return 0;
     }
     CloseHandle(hFile);
     DeleteFileA(testFile);
     return 1; // likely admin
 #else
     // macOS: we check if uid == 0 (root)
     return (getuid() == 0);
 #endif
 }
 
 //---------------------------------------------------------------------
 // Skip Folders
 //---------------------------------------------------------------------
 // Adjust as you wish. Case-insensitive checks. You might differ on Windows vs. mac
 // but let's keep them universal here for demonstration.
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
 static const int NUM_SKIP_FOLDERS = (int)(sizeof(SKIP_FOLDERS)/sizeof(SKIP_FOLDERS[0]));
 
 static int ShouldSkipFolder(const char *folderName)
 {
     if (!folderName || !*folderName) return 0;
     for (int i = 0; i < NUM_SKIP_FOLDERS; i++) {
 #if IS_WINDOWS
         if (_stricmp(folderName, SKIP_FOLDERS[i]) == 0) {
             return 1;
         }
 #else
         // On macOS, strcasecmp
         if (strcasecmp(folderName, SKIP_FOLDERS[i]) == 0) {
             return 1;
         }
 #endif
     }
     return 0;
 }
 
 //---------------------------------------------------------------------
 // Listing "drives"
 //  - Windows: GetLogicalDriveStrings, filter DRIVE_FIXED or DRIVE_REMOVABLE
 //  - macOS: list directories in /Volumes
 //---------------------------------------------------------------------
 
 #if IS_WINDOWS
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
             // "C:\"
             strncpy(drivesList[count], p, 3);
             drivesList[count][3] = '\0';
             count++;
         }
         p += strlen(p) + 1;
     }
     return count;
 }
 #else
 // macOS
 // We'll place the "volumes" in a drivesList, but each up to 100 chars or so.
 static int ListDrives(char drivesList[][PATH_MAX], int maxCount)
 {
     // We'll look in /Volumes
     const char *volPath = "/Volumes";
     DIR *d = opendir(volPath);
     if (!d) {
         return 0;
     }
     int count = 0;
     struct dirent *ent;
     while ((ent = readdir(d)) != NULL && count < maxCount) {
         // skip "." ".."
         if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
             continue;
         }
         // Build a full path: /Volumes/<NAME>
         char full[PATH_MAX];
         snprintf(full, sizeof(full), "%s/%s", volPath, ent->d_name);
         // We'll do a quick check if it's a directory (possible volume).
         struct stat st;
         if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
             // We'll add it as a "drive"
             strncpy(drivesList[count], full, PATH_MAX - 1);
             drivesList[count][PATH_MAX - 1] = '\0';
             count++;
         }
     }
     closedir(d);
     return count;
 }
 #endif
 
 //---------------------------------------------------------------------
 // Volume ID
 //---------------------------------------------------------------------
 static unsigned long GetVolumeID(const char *drivePath)
 {
 #if IS_WINDOWS
     // drivePath like "E:\" or "C:\"
     DWORD serial = 0;
     GetVolumeInformationA(drivePath, NULL, 0, &serial, NULL, NULL, NULL, 0);
     return (unsigned long)serial;
 #else
     // macOS: we can do statfs to get f_fsid
     struct statfs sfs;
     if (statfs(drivePath, &sfs) == 0) {
         // sfs.f_fsid is a typedef of fsid_t, typically two 32-bit values
         // We'll just XOR them to get a single 32-bit "ID".
         // Alternatively, you could store both 32-bit parts in volume_id.txt.
         unsigned long id = (unsigned long)(sfs.f_fsid.val[0] ^ sfs.f_fsid.val[1]);
         return id;
     }
     return 0;
 #endif
 }
 
 // write/read from backupFolder/volume_id.txt
 static int WriteVolumeID(const char *folder, unsigned long id)
 {
     char path[PATH_MAX];
 #if IS_WINDOWS
     _snprintf(path, PATH_MAX, "%s\\volume_id.txt", folder);
 #else
     snprintf(path, PATH_MAX, "%s/volume_id.txt", folder);
 #endif
     FILE *f = fopen(path, "w");
     if (!f) return 0;
     fprintf(f, "%lu", id);
     fclose(f);
     return 1;
 }
 
 static unsigned long ReadVolumeID(const char *folder)
 {
     char path[PATH_MAX];
 #if IS_WINDOWS
     _snprintf(path, PATH_MAX, "%s\\volume_id.txt", folder);
 #else
     snprintf(path, PATH_MAX, "%s/volume_id.txt", folder);
 #endif
     FILE *f = fopen(path, "r");
     if (!f) return 0;
     unsigned long val = 0;
     fscanf(f, "%lu", &val);
     fclose(f);
     return val;
 }
 
 //---------------------------------------------------------------------
 // Check if source is newer than destination
 //---------------------------------------------------------------------
 static int SourceIsNewer(const char *src, const char *dst)
 {
     struct STAT srcStat, dstStat;
     if (STAT(src, &srcStat) != 0) {
         // can't read src => let's say no
         return 0;
     }
     if (STAT(dst, &dstStat) != 0) {
         // dest doesn't exist => definitely copy
         return 1;
     }
     // Compare mod times
 #if IS_WINDOWS
     // st_mtime is the last-modified time
     return (srcStat.st_mtime > dstStat.st_mtime) ? 1 : 0;
 #else
     // same on mac
     return (srcStat.st_mtime > dstStat.st_mtime) ? 1 : 0;
 #endif
 }
 
 //---------------------------------------------------------------------
 // We will store a list of (src, dst) pairs to copy
 //---------------------------------------------------------------------
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
 
 //---------------------------------------------------------------------
 // Recursively enumerate source => build (src,dst) list
 //---------------------------------------------------------------------
 static void RecurseEnumerate(const char *srcDir, const char *dstDir)
 {
 #if IS_WINDOWS
     char searchPath[PATH_MAX];
     _snprintf(searchPath, PATH_MAX, "%s\\*", srcDir);
 
     WIN32_FIND_DATAA fdata;
     HANDLE hFind = FindFirstFileA(searchPath, &fdata);
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
 #else
     // macOS / POSIX
     DIR *d = opendir(srcDir);
     if (!d) return;
 
     struct dirent *ent;
     while ((ent = readdir(d)) != NULL) {
         const char *name = ent->d_name;
         if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
             continue;
         }
         char fullSrc[PATH_MAX];
         snprintf(fullSrc, sizeof(fullSrc), "%s/%s", srcDir, name);
 
         struct stat st;
         if (stat(fullSrc, &st) != 0) {
             continue;
         }
         if (S_ISDIR(st.st_mode)) {
             if (!ShouldSkipFolder(name)) {
                 char fullDst[PATH_MAX];
                 snprintf(fullDst, sizeof(fullDst), "%s/%s", dstDir, name);
                 RecurseEnumerate(fullSrc, fullDst);
             }
         } else {
             char fullDst[PATH_MAX];
             snprintf(fullDst, sizeof(fullDst), "%s/%s", dstDir, name);
             AddFilePair(fullSrc, fullDst);
         }
     }
     closedir(d);
 #endif
 }
 
 //---------------------------------------------------------------------
 // Create parent directories if needed
 //---------------------------------------------------------------------
 static void CreateParentDir(const char *dst)
 {
     // We'll do a naive approach: find the last slash, mkdir the path before it, and so forth.
     // For truly nested paths we might need to walk from the top. Here's a simplistic version:
     char tmp[PATH_MAX];
     strncpy(tmp, dst, PATH_MAX - 1);
     tmp[PATH_MAX - 1] = '\0';
 
 #if IS_WINDOWS
     char *p = strrchr(tmp, '\\');
     char slash = '\\';
     if (!p) {
         p = strrchr(tmp, '/'); 
         slash = '/';
     }
 #else
     char *p = strrchr(tmp, '/');
     char slash = '/';
 #endif
     if (!p) return; // no directory part
 
     *p = '\0';  // isolate the directory
     // We need to mkdir everything up to this path. Let's do a simple approach:
     // The "proper" approach on POSIX might be to call mkdir repeatedly segment by segment.
     // We'll do a loop approach.
     char *sep = tmp;
     while (1) {
         char *slashPos = strchr(sep, slash);
         if (!slashPos) {
             // last segment
             if (*tmp) {
                 MKDIR(tmp); // if it exists, it fails, but that's okay
             }
             break;
         }
         // temporarily break the string
         *slashPos = '\0';
         if (*tmp) {
             MKDIR(tmp);
         }
         *slashPos = slash;
         sep = slashPos + 1;
     }
 }
 
 //---------------------------------------------------------------------
 // Actual file copy
 //---------------------------------------------------------------------
 static int CopyOneFile(const char *src, const char *dst)
 {
 #if IS_WINDOWS
     // We can use CopyFileA
     CreateParentDir(dst);
     if (!CopyFileA(src, dst, FALSE)) {
         return 0; // fail
     }
     return 1;
 #else
     // macOS: do a manual copy with fopen/fread/fwrite, or use something like sendfile.
     // We'll do a simple read/write approach:
     CreateParentDir(dst);
 
     FILE *fin = fopen(src, "rb");
     if (!fin) return 0;
     FILE *fout = fopen(dst, "wb");
     if (!fout) {
         fclose(fin);
         return 0;
     }
     char buf[8192];
     size_t n;
     int success = 1;
     while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
         if (fwrite(buf, 1, n, fout) < n) {
             success = 0;
             break;
         }
     }
     fclose(fin);
     fclose(fout);
     return success;
 #endif
 }
 
 //---------------------------------------------------------------------
 // Multi-threading
 //---------------------------------------------------------------------
 #if IS_WINDOWS
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
 
 #else // macOS / pthread
 static volatile long gNextIndex   = 0;
 static volatile long gFilesCopied = 0;
 static volatile long gFilesFailed = 0;
 static volatile long gFilesSkipped= 0;
 static volatile long gFilesDone   = 0;
 static volatile long gFilesTotal  = 0;
 
 // We’ll define a small mutex or use atomic builtins. We'll do a simple __sync builtins for the index:
 static pthread_mutex_t gIndexMutex = PTHREAD_MUTEX_INITIALIZER;
 
 void* WorkerThread(void *arg)
 {
     (void)arg; // unused
     for (;;) {
         long idx;
         // Atomic or locked fetch of next index:
         pthread_mutex_lock(&gIndexMutex);
         idx = gNextIndex;
         gNextIndex++;
         pthread_mutex_unlock(&gIndexMutex);
 
         if (idx >= gFilesTotal) {
             break;
         }
         const char *src = gFileArray[idx].src;
         const char *dst = gFileArray[idx].dst;
 
         if (SourceIsNewer(src, dst)) {
             if (CopyOneFile(src, dst)) {
                 __sync_add_and_fetch(&gFilesCopied, 1);
             } else {
                 __sync_add_and_fetch(&gFilesFailed, 1);
             }
         } else {
             __sync_add_and_fetch(&gFilesSkipped, 1);
         }
         __sync_add_and_fetch(&gFilesDone, 1);
     }
     return NULL;
 }
 #endif
 
 //---------------------------------------------------------------------
 // MAIN
 //---------------------------------------------------------------------
 int main(void)
 {
     // 1) Check admin
     if (!IsRunningAsAdmin()) {
 #if IS_WINDOWS
         PrintError("Please run this program as Administrator (right-click -> Run as administrator).");
 #else
         PrintError("Please run this program as root (sudo) on macOS.");
 #endif
         printf("Press ENTER to exit...\n");
         getchar();
         return 1;
     }
 
     // 2) List drives
 #if IS_WINDOWS
     char drivesList[26][4]; // up to 26
     int driveCount = ListDrives(drivesList, 26);
 #else
     char drivesList[32][PATH_MAX]; // allow up to 32 volumes in /Volumes
     int driveCount = ListDrives(drivesList, 32);
 #endif
 
     if (driveCount == 0) {
         PrintError("No drives/volumes detected.");
         printf("Press ENTER to exit...\n");
         getchar();
         return 1;
     }
 
     PrintInfo("Detected drives/volumes:");
     for (int i = 0; i < driveCount; i++) {
         printf("  [%d] %s\n", i + 1, drivesList[i]);
     }
 
     printf("Which volume would you like to back up? (1 - %d): ", driveCount);
     int choice = 0;
     if (scanf("%d", &choice) != 1 || choice < 1 || choice > driveCount) {
         PrintError("Invalid choice. Aborting.");
         // consume leftover
         getchar(); getchar();
         return 1;
     }
     // chosen
 #if IS_WINDOWS
     char chosenDrive[4];
     strcpy(chosenDrive, drivesList[choice - 1]);
     // e.g. "E:\"
 #else
     char chosenDrive[PATH_MAX];
     strcpy(chosenDrive, drivesList[choice - 1]);
     // e.g. "/Volumes/MyUSB"
 #endif
 
     // confirm
     printf("You chose: %s. Continue? (Y/N): ", chosenDrive);
     char c;
     scanf(" %c", &c);
     if (tolower(c) != 'y') {
         PrintWarning("Backup canceled by user.");
         getchar(); getchar();
         return 0;
     }
 
     // 3) Build backup folder "backup_<whatever>"
     char backupFolder[PATH_MAX];
 #if IS_WINDOWS
     // If chosenDrive is "C:\" => letter is 'C'
     char letter = toupper(chosenDrive[0]);
     char currentDir[PATH_MAX];
     GetCurrentDirectoryA(PATH_MAX, currentDir);
     _snprintf(backupFolder, PATH_MAX, "%s\\backup_%c", currentDir, letter);
 #else
     // On mac, let's parse the last component of chosenDrive to use in the backup folder name
     // chosenDrive is something like "/Volumes/MyUSB"
     const char *p = strrchr(chosenDrive, '/');
     const char *volumeName = (p) ? (p+1) : chosenDrive;
     char cwd[PATH_MAX];
     getcwd(cwd, sizeof(cwd));
     snprintf(backupFolder, PATH_MAX, "%s/backup_%s", cwd, volumeName);
 #endif
 
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
         // not found => first time
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
     free(gFileArray); // in case re-run
     gFileArray = NULL;
 
     RecurseEnumerate(chosenDrive, backupFolder);
     if (gFileCount == 0) {
         PrintWarning("No files found or no access.");
         printf("Press ENTER to exit.\n");
         getchar(); getchar();
         return 0;
     }
     printf("Found %d files.\n", gFileCount);
 
     // 6) Multi-thread copy
 #if IS_WINDOWS
     gNextIndex   = 0;
     gFilesCopied = 0;
     gFilesFailed = 0;
     gFilesSkipped= 0;
     gFilesDone   = 0;
     gFilesTotal  = gFileCount;
 
     SYSTEM_INFO si;
     GetSystemInfo(&si);
     int numCores = si.dwNumberOfProcessors;
     if (numCores < 1) numCores = 1;
     if (numCores > 16) numCores = 16; // arbitrary cap
     printf("Using %d threads...\n", numCores);
 
     HANDLE *threads = (HANDLE*)malloc(numCores * sizeof(HANDLE));
     for (int i = 0; i < numCores; i++) {
         threads[i] = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
     }
 
     // main thread show progress
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
 
 #else
     // macOS / pthread
     gNextIndex   = 0;
     gFilesCopied = 0;
     gFilesFailed = 0;
     gFilesSkipped= 0;
     gFilesDone   = 0;
     gFilesTotal  = gFileCount;
 
     // how many cores?
     int numCores = (int)sysconf(_SC_NPROCESSORS_ONLN);
     if (numCores < 1) numCores = 1;
     if (numCores > 16) numCores = 16; // arbitrary cap
     printf("Using %d threads...\n", numCores);
 
     pthread_t *threads = (pthread_t*)malloc(numCores * sizeof(pthread_t));
     for (int i = 0; i < numCores; i++) {
         pthread_create(&threads[i], NULL, WorkerThread, NULL);
     }
 
     // main thread show progress
     while (1) {
         long done = gFilesDone;
         if (gFilesTotal > 0) {
             int percent = (int)((done * 100) / gFilesTotal);
             printf("\rProgress: %3d%% (%ld/%ld)", percent, done, (long)gFilesTotal);
             fflush(stdout);
         }
         if (done >= gFilesTotal) {
             break;
         }
         usleep(250000); // 0.25s
     }
     printf("\n");
 
     for (int i = 0; i < numCores; i++) {
         pthread_join(threads[i], NULL);
     }
     free(threads);
 
     long copied  = gFilesCopied;
     long failed  = gFilesFailed;
     long skipped = gFilesSkipped;
 #endif
 
     // summary
     if (failed == 0) {
         if (copied > 0) {
             char msg[256];
 #if IS_WINDOWS
             _snprintf(msg, sizeof(msg), 
 #else
             snprintf(msg, sizeof(msg),
 #endif
                 "Backup complete! Copied %ld files; %ld were already up-to-date.",
                 copied, skipped);
             PrintSuccess(msg);
         } else {
             PrintInfo("All files were already up-to-date. Nothing needed copying.");
         }
     } else {
         char msg[256];
 #if IS_WINDOWS
         _snprintf(msg, sizeof(msg),
 #else
         snprintf(msg, sizeof(msg),
 #endif
             "Backup finished with errors. Copied:%ld Failed:%ld Skipped(up-to-date):%ld",
             copied, failed, skipped);
         PrintWarning(msg);
     }
 
     printf("\nPress ENTER to exit...\n");
     getchar(); getchar();
 
     free(gFileArray);
     gFileArray = NULL;
     return 0;
 }
 