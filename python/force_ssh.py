import os
import sys
import platform
import subprocess

def enable_ssh_all_os_headless():
    """
    Attempts (non-interactively) to:
      1) Check if already running with admin/root privileges.
      2) Enable SSH on Linux, macOS, or Windows without prompting.

    Returns:
        bool: True if SSH was successfully enabled (or is already running).
              False if something failed or if not elevated.
    """

    def is_admin():
        """
        Check if we have administrative privileges.
        - On Windows, try writing to %SystemRoot%\\Temp.
        - On Linux/macOS, check if effective UID is 0.
        """
        system_name = platform.system().lower()
        if system_name == "windows":
            try:
                temp_dir = os.path.join(os.environ.get("SystemRoot", "C:\\Windows"), "Temp")
                test_path = os.path.join(temp_dir, "test_admin.txt")
                with open(test_path, "w") as f:
                    f.write("admin_test")
                os.remove(test_path)
                return True
            except PermissionError:
                return False
            except Exception:
                return False
        else:
            return (os.geteuid() == 0)

    def do_enable_ssh():
        """
        Enable SSH on Linux, macOS, or Windows (headless).
        Returns True if successful, False otherwise.
        """
        os_name = platform.system().lower()

        # Common kwargs for subprocess calls (no interactive prompts, discard stdout/stderr)
        run_opts = {
            "check": True,
            "stdin": subprocess.DEVNULL,
            "stdout": subprocess.DEVNULL,
            "stderr": subprocess.DEVNULL,
        }

        # ---------- Linux ----------
        if os_name == "linux":
            try:
                # Check systemctl
                subprocess.run(["systemctl", "--version"], **run_opts)
            except (FileNotFoundError, subprocess.CalledProcessError):
                return False

            # Try possible service names
            for service_name in ["ssh", "sshd"]:
                try:
                    subprocess.run(["systemctl", "enable", service_name], **run_opts)
                    subprocess.run(["systemctl", "start", service_name], **run_opts)
                    # Check if it's active
                    status_cmd = subprocess.run(
                        ["systemctl", "is-active", service_name],
                        capture_output=True,
                        text=True
                    )
                    if status_cmd.returncode == 0 and "active" in status_cmd.stdout.strip():
                        return True
                except subprocess.CalledProcessError:
                    pass
            return False

        # ---------- macOS ----------
        elif os_name == "darwin":
            # Try enabling Remote Login
            try:
                result = subprocess.run(
                    ["systemsetup", "-setremotelogin", "on"],
                    capture_output=True,
                    text=True
                )
                return (result.returncode == 0)
            except (FileNotFoundError, subprocess.CalledProcessError):
                return False

        # ---------- Windows ----------
        elif os_name == "windows":
            ps_base = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command"]

            try:
                # 1) Check if OpenSSH.Server is installed
                check_cmd = ps_base + [
                    "Get-WindowsCapability -Online | "
                    "Where-Object { $_.Name -like 'OpenSSH.Server*' } | "
                    "Select-Object -ExpandProperty State"
                ]
                check_install = subprocess.run(check_cmd, capture_output=True, text=True)
                # If "NotPresent", install it
                if "NotPresent" in check_install.stdout:
                    install_cmd = ps_base + [
                        "Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0"
                    ]
                    subprocess.run(install_cmd, **run_opts)

                # 2) Enable & start sshd
                set_service_cmd = ps_base + ["Set-Service -Name sshd -StartupType Automatic"]
                subprocess.run(set_service_cmd, **run_opts)

                start_service_cmd = ps_base + ["Start-Service sshd"]
                subprocess.run(start_service_cmd, **run_opts)

                return True
            except (subprocess.CalledProcessError, FileNotFoundError):
                return False

        # ---------- Unsupported OS ----------
        else:
            return False

    # --- Main logic ---
    # If not admin, do not attempt any elevation (headless), just return False
    if not is_admin():
        return False

    # We are in admin/root context. Proceed to enable SSH.
    return do_enable_ssh()
