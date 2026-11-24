"""Darwin restrictions shared by local package staging tools."""
import stat
import subprocess


def check_file_flags(st):
    protected = (stat.SF_RESTRICTED | stat.SF_IMMUTABLE | stat.SF_APPEND | stat.SF_NOUNLINK |
                 stat.UF_IMMUTABLE | stat.UF_APPEND | stat.UF_NOUNLINK | stat.UF_DATAVAULT)
    if st.st_flags & protected:
        raise ValueError("protected Darwin file flags require the original filesystem context")


def check_quarantine(source):
    for node in (source, *source.parents):
        if "com.apple.quarantine" in subprocess.check_output(["/usr/bin/xattr", str(node)], text=True).splitlines():
            raise ValueError("quarantine assessment required")
