"""Check that invalid pool settings are rejected before any database I/O."""

import pathlib
import resource
import shutil
import subprocess
import sys
import tempfile


def replace_setting(config, key, value):
    lines = config.splitlines()
    for index, line in enumerate(lines):
        if line.startswith(key + "="):
            lines[index] = key + "=" + value
            break
    else:
        lines.append(key + "=" + value)
    return "\n".join(lines) + "\n"


def main(executable, config_file):
    # 无效配置会沿用构造函数的 abort 策略；避免测试进程写出 core 文件。
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source_config = pathlib.Path(config_file).read_text()
    cases = {
        "capacity": ("maxSize", "1"),
        "idle time": ("maxIdleTime", "0"),
        "borrow timeout": ("ConnectionTimeOut", "0"),
        "port": ("port", "0"),
        "malformed number": ("initSize", "10junk"),
        "connect timeout": ("connectTimeout", "0"),
        "read timeout": ("readTimeout", "0"),
        "write timeout": ("writeTimeout", "0"),
    }
    with tempfile.TemporaryDirectory(prefix="tlss_pool_config_") as folder:
        root = pathlib.Path(folder)
        binary = root / "bin" / "mysql_conn_test"
        binary.parent.mkdir()
        (root / "sql").mkdir()
        shutil.copy2(executable, binary)
        for name, (key, value) in cases.items():
            (root / "sql" / "mysql.cnf").write_text(
                replace_setting(source_config, key, value)
            )
            result = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=5
            )
            output = result.stdout + result.stderr
            assert result.returncode != 0 and "Invalid MySQL configuration" in output, (
                f"{name}: expected configuration rejection, got exit {result.returncode}"
            )
            print(name + ": rejected")

        if len(sys.argv) > 3 and sys.argv[3] == "--valid":
            # 有数据库可用时，再验证新增网络超时配置可正常建立连接。
            valid_config = source_config
            for key in ("connectTimeout", "readTimeout", "writeTimeout"):
                valid_config = replace_setting(valid_config, key, "2")
            (root / "sql" / "mysql.cnf").write_text(valid_config)
            result = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=8
            )
            assert result.returncode == 0, (
                f"valid network timeout settings: exit {result.returncode}"
            )
            print("valid network timeouts: accepted")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
