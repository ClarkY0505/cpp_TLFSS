"""Keep the business API usable without TCP or process resource ownership."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "inc"


def header_dependencies(entry):
    pending = [entry]
    seen = set()
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        text = (INCLUDE / path).read_text()
        for include in re.findall(r'^\s*#include\s+["<]([^">]+)[">]', text, re.M):
            if (INCLUDE / include).is_file():
                pending.append(include)
            elif (INCLUDE / path).parent.joinpath(include).is_file():
                pending.append(str(Path(path).parent / include))
    return seen


class BusinessBoundaryTest(unittest.TestCase):
    def test_public_business_dependencies_do_not_require_tcp_or_pools(self):
        forbidden = (
            "common/net/",
            "common/base/mysql_connection",
            "common/base/tlss_thread",
            "ftp/context.h",
            "ftp/transfer_task.h",
            "ftp/async_file_",
            "ftp/disk_executor.h",
        )
        for entry in ("ftp/server.h", "ftp/upload_engine.h", "ftp/download_engine.h"):
            with self.subTest(entry=entry):
                dependencies = header_dependencies(entry)
                violations = sorted(path for path in dependencies if path.startswith(forbidden))
                self.assertEqual(violations, [], f"{entry} leaks transport/process resources")


if __name__ == "__main__":
    unittest.main()
