/*
 * backup_drive.c
 *
 * A cleaner, more user-friendly, Windows-only console program that:
 *   1) Checks for admin privileges.
 *   2) Lists all fixed drives (C:, D:, E:, etc.).
 *   3) Prompts user to choose one drive.
 *   4) Asks for confirmation, then backs up that drive's entire contents into
 *      "backup_<drive_letter>" in the current directory.
 *   5) Skips certain folder names (customizable).
 *   6) Copies only new or changed files (compares modification times).
 *   7) Shows % progress.
 *   8) Has improved error handling and a slightly nicer console interface.
 */

 #include <windows.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <ctype.h>
 #include <direct.h>  // _mkdir
 #include <sys/stat.h>
 
 #pragma comment(lib, "Advapi32.lib")
 
 #ifndef PATH_MAX
   #define PATH_MAX 260
 #endif
 
 #define ACCESS _access
 #define MKDIR(a) _mkdir(a)
 #define STAT _stat
 typedef struct _stat stat_t;
 
 /*----------------------------------------------------------------------
  *                     CONSOLE COLOR UTILS (OPTIONAL)
  *  Feel free to disable color if you want absolutely no Win32 calls.
  *  But this is standard for Windows console and requires no extra DLL.
  *---------------------------------------------------------------------*/
 static void SetConsoleColor(WORD attribs)
 {
     HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
     if (hConsole != INVALID_HANDLE_VALUE) {
         SetConsoleTextAttribute(hConsole, attribs);
     }
 }
 
 static void ResetConsoleColor(void)
 {
     // White (bright) on black is typical default
     SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
 }
 
 static void PrintError(const char *msg)
 {
     SetConsoleColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
     fprintf(stderr, "[ERROR] %s\n", msg);
     ResetConsoleColor();
 }
 
 static void PrintWarning(const char *msg)
 {
     SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
     fprintf(stderr, "[WARNING] %s\n", msg);
     ResetConsoleColor();
 }
 
 static void PrintInfo(const char *msg)
 {
     SetConsoleColor(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
     printf("[INFO] %s\n", msg);
     ResetConsoleColor();
 }
 
 static void PrintSuccess(const char *msg)
 {
     SetConsoleColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
     printf("[SUCCESS] %s\n", msg);
     ResetConsoleColor();
 }
 
 /*----------------------------------------------------------------------
  *                        CONFIGURATION
  *---------------------------------------------------------------------*/
 
 /*
  * List of folder names to skip entirely (case-insensitive).
  * Adjust or extend as you wish.
  */
 static const char *SKIP_FOLDERS[] = {
     "$Recycle.Bin",
     "Windows",
     "Program Files",
     "Program Files (x86)",
     "ProgramData",
     "System Volume Information",
     "backup_C",
     "backup_D",
     "backup_E"
 };
 static const int NUM_SKIP_FOLDERS = (int)(sizeof(SKIP_FOLDERS)/sizeof(SKIP_FOLDERS[0]));
 
 /*----------------------------------------------------------------------
  *                   ADMIN CHECK
  *---------------------------------------------------------------------*/
 static int IsAdmin(void)
 {
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
         return 0;  // Not admin
     }
     CloseHandle(hFile);
     DeleteFileA(testFile);
     return 1;      // Admin
 }
 
 /*----------------------------------------------------------------------
  *               DRIVE LISTING + USER SELECTION
  *---------------------------------------------------------------------*/
 
 /*
  * Returns all "fixed" drives, e.g. "C:\", "D:\", ...
  * We store them in the array `drivesList[][4]`, up to maxCount entries.
  * Returns how many were found.
  */
 static int ListFixedDrives(char drivesList[][4], int maxCount)
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
         if (dt == DRIVE_FIXED) {
             // p might be "C:\"
             strncpy(drivesList[count], p, 3);   // copy "C:\"
             drivesList[count][3] = '\0';       // ensure null-terminated
             count++;
         }
         p += strlen(p) + 1;
     }
     return count;
 }
 
 /*----------------------------------------------------------------------
  *               SKIP FOLDERS CHECK
  *---------------------------------------------------------------------*/
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
 
 /*----------------------------------------------------------------------
  *              COPY A FILE IF NEW OR CHANGED
  *---------------------------------------------------------------------*/
 /*
  * Copy src -> dst, overwriting if needed.
  * Returns 1 on success, 0 on failure.
  */
 static int CopyFileSimple(const char *src, const char *dst)
 {
     FILE *fin = fopen(src, "rb");
     if (!fin) {
         return 0;
     }
 
     // Make parent dir (only one level deep).
     {
         char tmp[PATH_MAX];
         strncpy(tmp, dst, PATH_MAX-1);
         tmp[PATH_MAX-1] = '\0';
 
         char *p = strrchr(tmp, '\\');
         if (!p) p = strrchr(tmp, '/');
         if (p) {
             *p = '\0';
             // Attempt to create directory.
             // If it fails because it exists, that's fine.
             MKDIR(tmp);
         }
     }
 
     FILE *fout = fopen(dst, "wb");
     if (!fout) {
         fclose(fin);
         return 0;
     }
 
     // Copy loop
     char buf[8192];
     size_t n;
     while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
         if (fwrite(buf, 1, n, fout) < n) {
             // Write error
             fclose(fin);
             fclose(fout);
             return 0;
         }
     }
     fclose(fin);
     fclose(fout);
     return 1;
 }
 
 /*
  * Copy only if dst doesn't exist or src is newer.
  * Returns:
  *   1 if a copy occurred successfully,
  *   2 if skip (no copy needed),
  *   0 on error (wanted to copy, but failed).
  */
 static int BackupFileIfNewOrChanged(const char *srcPath, const char *dstPath)
 {
     stat_t srcStat, dstStat;
     if (STAT(srcPath, &srcStat) != 0) {
         // Source unreadable
         return 0;
     }
     if (STAT(dstPath, &dstStat) != 0) {
         // Destination doesn't exist => copy
         return CopyFileSimple(srcPath, dstPath) ? 1 : 0;
     } else {
         // Compare mod times
         if (srcStat.st_mtime > dstStat.st_mtime) {
             // Source is newer => copy
             return CopyFileSimple(srcPath, dstPath) ? 1 : 0;
         } else {
             // Already up-to-date => skip
             return 2;
         }
     }
 }
 
 /*----------------------------------------------------------------------
  *                    FILE COUNTING / PROGRESS
  *---------------------------------------------------------------------*/
 /*
  * Count how many regular files we will attempt to process,
  * skipping any folder in ShouldSkipFolder.
  */
 static int CountFiles(const char *dir)
 {
     char searchPath[PATH_MAX];
     _snprintf(searchPath, PATH_MAX, "%s\\*", dir);
 
     WIN32_FIND_DATAA findData;
     HANDLE hFind = FindFirstFileA(searchPath, &findData);
     if (hFind == INVALID_HANDLE_VALUE) {
         return 0;
     }
 
     int total = 0;
     do {
         const char *name = findData.cFileName;
         if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
             continue;
         }
         char fullPath[PATH_MAX];
         _snprintf(fullPath, PATH_MAX, "%s\\%s", dir, name);
 
         if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
             if (!ShouldSkipFolder(name)) {
                 total += CountFiles(fullPath);
             }
         } else {
             total++;
         }
     } while (FindNextFileA(hFind, &findData) != 0);
 
     FindClose(hFind);
     return total;
 }
 
 /*
  * Recursively process all items under srcDir -> dstDir,
  * skipping ShouldSkipFolder. Increments *filesProcessed
  * and updates progress for each file encountered.
  * Also counts how many were actually copied vs. how many failed, etc.
  */
 static void RecurseBackup(const char *srcDir,
                           const char *dstDir,
                           int *filesProcessed,
                           int totalFiles,
                           int *filesCopied,
                           int *filesFailed)
 {
     char searchPath[PATH_MAX];
     _snprintf(searchPath, PATH_MAX, "%s\\*", srcDir);
 
     WIN32_FIND_DATAA findData;
     HANDLE hFind = FindFirstFileA(searchPath, &findData);
     if (hFind == INVALID_HANDLE_VALUE) {
         // Possibly no access, or folder doesn't exist
         return;
     }
 
     do {
         const char *name = findData.cFileName;
         if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
             continue;
         }
         char fullSrc[PATH_MAX];
         _snprintf(fullSrc, PATH_MAX, "%s\\%s", srcDir, name);
 
         if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
             if (!ShouldSkipFolder(name)) {
                 char fullDst[PATH_MAX];
                 _snprintf(fullDst, PATH_MAX, "%s\\%s", dstDir, name);
                 RecurseBackup(fullSrc, fullDst, filesProcessed, totalFiles,
                               filesCopied, filesFailed);
             }
         } else {
             // It's a file
             (*filesProcessed)++;
             char fullDst[PATH_MAX];
             _snprintf(fullDst, PATH_MAX, "%s\\%s", dstDir, name);
 
             int rc = BackupFileIfNewOrChanged(fullSrc, fullDst);
             if (rc == 1) {
                 (*filesCopied)++;
             } else if (rc == 0) {
                 (*filesFailed)++;
             }
             // Update progress
             if (totalFiles > 0) {
                 int percent = (*filesProcessed * 100) / totalFiles;
                 printf("\rProgress: %3d%% (%d/%d)", percent, *filesProcessed, totalFiles);
                 fflush(stdout);
             }
         }
     } while (FindNextFileA(hFind, &findData) != 0);
 
     FindClose(hFind);
 }
 
 /*----------------------------------------------------------------------
  *                         MAIN
  *---------------------------------------------------------------------*/
 int main(void)
 {
     // 1) Must be admin
     if (!IsAdmin()) {
         PrintError("You must run this program as Administrator.");
         printf("Press any key to exit...\n");
         getchar();
         return 1;
     }
 
     // 2) List all fixed drives
     char drivesList[26][4];  // up to 26
     int driveCount = ListFixedDrives(drivesList, 26);
     if (driveCount == 0) {
         PrintError("No fixed drives detected on this system.");
         printf("Press any key to exit...\n");
         getchar();
         return 1;
     }
 
     PrintInfo("Detected fixed drives:");
     for (int i = 0; i < driveCount; i++) {
         printf("  [%d] %s\n", i + 1, drivesList[i]); 
     }
 
     // 3) Prompt user to select
     printf("\nWhich drive would you like to back up? (1 - %d): ", driveCount);
     int choice = 0;
     if (scanf("%d", &choice) != 1 || choice < 1 || choice > driveCount) {
         PrintError("Invalid choice. Aborting.");
         printf("Press any key to exit...\n");
         getchar(); getchar(); // flush leftover
         return 1;
     }
     // Chosen drive (like "C:\")
     char chosenDrive[4];
     strcpy(chosenDrive, drivesList[choice - 1]);
 
     // 4) Confirm the choice
     printf("You have chosen drive %s. Continue backup? (Y/N): ", chosenDrive);
     char confirm = 0;
     scanf(" %c", &confirm); // read a single char with leading whitespace
     if (tolower(confirm) != 'y') {
         PrintWarning("Backup canceled by user.");
         printf("Press any key to exit...\n");
         getchar(); getchar();
         return 0;
     }
 
     // 5) Determine current directory and create "backup_<letter>"
     char currentDir[PATH_MAX];
     if (!GetCurrentDirectoryA(PATH_MAX, currentDir)) {
         PrintError("Failed to get current directory.");
         printf("Press any key to exit...\n");
         getchar(); getchar();
         return 1;
     }
 
     // Build backup folder name. If chosenDrive is "C:\", then letter = 'C'
     char backupFolder[PATH_MAX];
     char letter = toupper(chosenDrive[0]);
     _snprintf(backupFolder, PATH_MAX, "%s\\backup_%c", currentDir, letter);
 
     // Attempt to create
     if (MKDIR(backupFolder) != 0) {
         // If it already exists, _mkdir might fail. We can check errno.
         if (errno != EEXIST) {
             PrintError("Failed to create backup folder.");
             printf("Press any key to exit...\n");
             getchar(); getchar();
             return 1;
         }
     }
 
     // 6) Pre-count how many files
     PrintInfo("Counting files... please wait.");
     int totalFiles = CountFiles(chosenDrive);
     if (totalFiles == 0) {
         PrintWarning("No files found on that drive or unable to access them.");
         printf("Press any key to exit...\n");
         getchar(); getchar();
         return 0;
     }
     printf("Found %d files to process.\n", totalFiles);
 
     // 7) Backup
     PrintInfo("Starting backup...");
     int filesProcessed = 0;
     int filesCopied    = 0;
     int filesFailed    = 0;
 
     RecurseBackup(chosenDrive, backupFolder,
                   &filesProcessed,
                   totalFiles,
                   &filesCopied,
                   &filesFailed);
 
     // Final progress line
     printf("\rProgress: 100%% (%d/%d)\n", filesProcessed, totalFiles);
 
     // 8) Summary
     printf("\n");
     if (filesFailed == 0) {
         if (filesCopied > 0) {
             char msg[256];
             _snprintf(msg, sizeof(msg),
                       "Backup complete! Copied %d files; %d were already up-to-date.",
                       filesCopied,
                       (filesProcessed - filesCopied));
             PrintSuccess(msg);
         } else {
             PrintInfo("All files were already up-to-date. Nothing needed copying.");
         }
     } else {
         char msg[256];
         _snprintf(msg, sizeof(msg),
                   "Backup finished with some errors. %d succeeded; %d failed; %d already up-to-date.",
                   filesCopied,
                   filesFailed,
                   (filesProcessed - filesCopied - filesFailed));
         PrintWarning(msg);
     }
 
     printf("\nPress any key to exit...\n");
     getchar(); getchar();
     return 0;
 } 