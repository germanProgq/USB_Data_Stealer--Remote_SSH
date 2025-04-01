/*
 * High-Performance Backup (POSIX-only, minimal dependencies)
 *
 * This program:
 *  - Checks if running as root (optional, but helps ensure file access).
 *  - Enumerates "drives" by scanning common mount points ("/Volumes", "/media", "/mnt", "/run/media").
 *    Adjust as needed for your environment.
 *  - Lets the user pick one volume to back up.
 *  - Stores volume ID in "backup_<volume>/volume_id.txt", uses `statfs()` to create a unique ID.
 *  - Skips certain folders by name (see SKIP_FOLDERS).
 *  - Copies files only if the source is newer or destination doesn't exist.
 *  - Uses multi-threading via pthread to gain higher performance.
 *  - Large-block copying (manual read/write in 64KB chunks).
 *
 * Compile:
 *   cc -O2 backup_minimal_posix.c -o backup_minimal_posix -lpthread
 *
 * If you truly lack pthread support, remove the multi-threaded parts (WorkerThread, etc.).
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <ctype.h>
 #include <errno.h>
 #include <unistd.h>
 #include <dirent.h>
 #include <sys/types.h>
 #include <sys/stat.h>
 #include <sys/statfs.h>
 #include <fcntl.h>
 #include <pthread.h>
 #include <time.h>
 
 #ifndef PATH_MAX
   #define PATH_MAX 1024
 #endif
 
 typedef struct stat stat_t;
 
 // ---------------------------------------------------------------------
 // Configuration
 // ---------------------------------------------------------------------
 
 // Skip these folders (case-insensitive match)
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
 
 // Check root (optional). If you want to allow non-root usage, remove.
 static int IsRunningAsRoot(void)
 {
     return (getuid() == 0);
 }
 
 // ---------------------------------------------------------------------
 // Utility: skip certain folders by name
 // ---------------------------------------------------------------------
 static int ShouldSkipFolder(const char *folderName)
 {
     if (!folderName || !*folderName) return 0;
     for (int i = 0; i < NUM_SKIP_FOLDERS; i++) {
         // case-insensitive compare
         if (strcasecmp(folderName, SKIP_FOLDERS[i]) == 0) {
             return 1;
         }
     }
     return 0;
 }
 
 // ---------------------------------------------------------------------
 // Attempt to list "drives"/volumes
 //   - On macOS: /Volumes
 //   - On Linux: /media, /mnt, /run/media
 // Adjust as your environment needs.
 // ---------------------------------------------------------------------
 static int ListDrives(char drivesList[][PATH_MAX], int maxCount)
 {
     int count = 0;
     // Potential mount dirs:
     const char *pathsToCheck[] = { "/Volumes", "/media", "/mnt", "/run/media" };
     size_t nPaths = sizeof(pathsToCheck)/sizeof(pathsToCheck[0]);
 
     for (size_t i = 0; i < nPaths; i++) {
         DIR *d = opendir(pathsToCheck[i]);
         if (!d) {
             continue; // skip if doesn't exist
         }
         struct dirent *ent;
         while ((ent = readdir(d)) != NULL) {
             // skip "." and ".."
             if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                 continue;
             }
             char full[PATH_MAX];
             snprintf(full, sizeof(full), "%s/%s", pathsToCheck[i], ent->d_name);
 
             struct stat st;
             if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                 // add as a "drive"
                 strncpy(drivesList[count], full, PATH_MAX - 1);
                 drivesList[count][PATH_MAX - 1] = '\0';
                 count++;
                 if (count >= maxCount) {
                     closedir(d);
                     return count;
                 }
             }
         }
         closedir(d);
     }
     return count;
 }
 
 // ---------------------------------------------------------------------
 // Volume ID from statfs->f_fsid
 //   - We'll XOR the two parts of the fsid
 // ---------------------------------------------------------------------
 static unsigned long GetVolumeID(const char *path)
 {
     struct statfs sfs;
     if (statfs(path, &sfs) == 0) {
         unsigned long val = (unsigned long)(sfs.f_fsid.__val[0] ^ sfs.f_fsid.__val[1]);
         return val;
     }
     return 0;
 }
 
 static int WriteVolumeID(const char *folder, unsigned long id)
 {
     char path[PATH_MAX];
     snprintf(path, PATH_MAX, "%s/volume_id.txt", folder);
     FILE *f = fopen(path, "w");
     if (!f) return 0;
     fprintf(f, "%lu", id);
     fclose(f);
     return 1;
 }
 
 static unsigned long ReadVolumeID(const char *folder)
 {
     char path[PATH_MAX];
     snprintf(path, PATH_MAX, "%s/volume_id.txt", folder);
     FILE *f = fopen(path, "r");
     if (!f) return 0;
     unsigned long val = 0;
     fscanf(f, "%lu", &val);
     fclose(f);
     return val;
 }
 
 // ---------------------------------------------------------------------
 // Is source newer than destination?
 // ---------------------------------------------------------------------
 static int SourceIsNewer(const char *src, const char *dst)
 {
     stat_t sStat, dStat;
     if (stat(src, &sStat) != 0) {
         return 0; // can't read source => skip
     }
     if (stat(dst, &dStat) != 0) {
         // dest doesn't exist => copy
         return 1;
     }
     return (sStat.st_mtime > dStat.st_mtime) ? 1 : 0;
 }
 
 // ---------------------------------------------------------------------
 // We'll store (src, dst) pairs
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
 static void CreateParentDir(const char *path)
 {
     // We'll parse `path`, isolate directory portion, then mkdir recursively
     char tmp[PATH_MAX];
     strncpy(tmp, path, PATH_MAX - 1);
     tmp[PATH_MAX - 1] = '\0';
 
     char *slash = strrchr(tmp, '/');
     if (!slash) return; // no directory portion
     *slash = '\0';
 
     // walk from left to right
     char *p = tmp;
     while (1) {
         char *sep = strchr(p, '/');
         if (!sep) {
             // last piece
             if (*tmp) {
                 mkdir(tmp, 0777); // ignore if exists
             }
             break;
         }
         // skip if it's the leading slash
         if (sep != tmp) {
             *sep = '\0';
             mkdir(tmp, 0777); // ignore if exists
             *sep = '/';
         }
         p = sep + 1;
     }
 }
 
 // ---------------------------------------------------------------------
 // Copy one file (large-buffer read/write)
 // ---------------------------------------------------------------------
 static int CopyOneFile(const char *src, const char *dst)
 {
     CreateParentDir(dst);
 
     int inFd = open(src, O_RDONLY);
     if (inFd < 0) {
         return 0;
     }
     int outFd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
     if (outFd < 0) {
         close(inFd);
         return 0;
     }
 
     char buf[65536]; // 64KB buffer
     ssize_t n;
     int success = 1;
     while ((n = read(inFd, buf, sizeof(buf))) > 0) {
         ssize_t w = write(outFd, buf, n);
         if (w < n) {
             success = 0;
             break;
         }
     }
     close(inFd);
     close(outFd);
     return success;
 }
 
 // ---------------------------------------------------------------------
 // Recursively enumerate source => build (src, dst) list
 // ---------------------------------------------------------------------
 static void RecurseEnumerate(const char *srcDir, const char *dstDir)
 {
     DIR *d = opendir(srcDir);
     if (!d) {
         return;
     }
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
         } else if (S_ISREG(st.st_mode)) {
             char fullDst[PATH_MAX];
             snprintf(fullDst, sizeof(fullDst), "%s/%s", dstDir, name);
             AddFilePair(fullSrc, fullDst);
         }
     }
     closedir(d);
 }
 
 // ---------------------------------------------------------------------
 // Multi-threading with pthread
 // ---------------------------------------------------------------------
 static pthread_mutex_t gIndexMutex = PTHREAD_MUTEX_INITIALIZER;
 static long gNextIndex   = 0;
 static long gFilesCopied = 0;
 static long gFilesFailed = 0;
 static long gFilesSkipped= 0;
 static long gFilesDone   = 0;
 static long gFilesTotal  = 0;
 
 static void* WorkerThread(void *arg)
 {
     (void)arg;
     while (1) {
         // get the next index
         pthread_mutex_lock(&gIndexMutex);
         long idx = gNextIndex++;
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
 
 // ---------------------------------------------------------------------
 // MAIN
 // ---------------------------------------------------------------------
 int main(void)
 {
     // 1) (Optional) check if root
     if (!IsRunningAsRoot()) {
         printf("ERROR: Please run as root (sudo) if you need full access.\n");
         // If you want to proceed anyway, remove this return.
         return 1;
     }
 
     // 2) List drives
     char drivesList[32][PATH_MAX];
     int driveCount = ListDrives(drivesList, 32);
     if (driveCount == 0) {
         printf("ERROR: No drives found in /Volumes, /media, /mnt, /run/media.\n");
         return 1;
     }
     printf("Detected volumes:\n");
     for (int i = 0; i < driveCount; i++) {
         printf("  [%d] %s\n", i+1, drivesList[i]);
     }
     printf("Choose a volume to back up (1..%d): ", driveCount);
     int choice;
     if (scanf("%d", &choice) != 1 || choice < 1 || choice > driveCount) {
         printf("Invalid choice.\n");
         return 1;
     }
     getchar(); // consume leftover newline
 
     // Chosen path
     char chosen[PATH_MAX];
     strcpy(chosen, drivesList[choice - 1]);
     printf("You chose: %s. Proceed? (Y/N): ", chosen);
     char c;
     if (scanf(" %c", &c) != 1 || tolower(c) != 'y') {
         printf("Aborted.\n");
         return 0;
     }
     getchar(); // leftover newline
 
     // 3) Build backup folder "backup_<basename>"
     const char *p = strrchr(chosen, '/');
     const char *namePart = (p ? p+1 : chosen);
     char cwd[PATH_MAX];
     getcwd(cwd, sizeof(cwd));
     char backupFolder[PATH_MAX];
     snprintf(backupFolder, sizeof(backupFolder), "%s/backup_%s", cwd, namePart);
 
     if (mkdir(backupFolder, 0777) != 0 && errno != EEXIST) {
         printf("ERROR: Could not create backup folder: %s\n", backupFolder);
         return 1;
     }
 
     // 4) volume ID check
     unsigned long volID = GetVolumeID(chosen);
     unsigned long existingID = ReadVolumeID(backupFolder);
     if (existingID == 0) {
         // first time
         WriteVolumeID(backupFolder, volID);
     } else if (existingID != volID) {
         printf("WARNING: This backup folder was used for a different volume previously.\n");
         printf("Proceed anyway? (Y/N): ");
         char ans;
         if (scanf(" %c", &ans) != 1 || tolower(ans) != 'y') {
             printf("Aborted.\n");
             return 0;
         }
     }
 
     // 5) Enumerate files
     gFileCount = 0;
     gFileCap   = 0;
     if (gFileArray) {
         free(gFileArray);
         gFileArray = NULL;
     }
 
     printf("Enumerating files...\n");
     RecurseEnumerate(chosen, backupFolder);
     if (gFileCount == 0) {
         printf("No files found or no access.\n");
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
 
     long numCores = sysconf(_SC_NPROCESSORS_ONLN);
     if (numCores < 1)  numCores = 1;
     if (numCores > 64) numCores = 64; // arbitrary cap
     printf("Using %ld threads.\n", numCores);
 
     pthread_t *threads = (pthread_t*)malloc(numCores * sizeof(pthread_t));
     for (int i = 0; i < numCores; i++) {
         pthread_create(&threads[i], NULL, WorkerThread, NULL);
     }
 
     // show progress
     while (1) {
         long done = __sync_add_and_fetch(&gFilesDone, 0);
         if (done >= gFilesTotal) {
             // all done
             break;
         }
         int percent = (int)((done * 100) / gFilesTotal);
         printf("\rProgress: %3d%% (%ld/%ld)", percent, done, gFilesTotal);
         fflush(stdout);
         usleep(200000); // 0.2s
     }
     printf("\rProgress: 100%% (%ld/%ld)\n", gFilesTotal, gFilesTotal);
 
     for (int i = 0; i < numCores; i++) {
         pthread_join(threads[i], NULL);
     }
     free(threads);
 
     long copied  = __sync_add_and_fetch(&gFilesCopied, 0);
     long failed  = __sync_add_and_fetch(&gFilesFailed, 0);
     long skipped = __sync_add_and_fetch(&gFilesSkipped, 0);
 
     // summary
     printf("\nSummary:\n");
     printf("  Copied:  %ld\n", copied);
     printf("  Failed:  %ld\n", failed);
     printf("  Skipped: %ld (already up-to-date)\n", skipped);
 
     if (failed == 0) {
         if (copied > 0) {
             printf("\nBackup complete! %ld files copied.\n", copied);
         } else {
             printf("\nAll files already up-to-date.\n");
         }
     } else {
         printf("\nBackup finished with errors.\n");
     }
     return 0;
 }
 