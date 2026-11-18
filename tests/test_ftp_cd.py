"""执行当前目录查询 SQL；路径解析和结果校验由 C++ 服务测试覆盖。"""

import json
from pathlib import Path
import re
import sqlite3
import unittest


class DirectoryLookupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (
            Path(__file__).resolve().parents[1]
            / "src/ftp/metadata_service.cpp"
        ).read_text()
        # 只从当前私有查询方法提取 SQL，避免误取文件中的其他元数据查询。
        function = source.split("MetadataService::find_directory_by_path_db(", 1)[1]
        function = function.split("\n}\n", 1)[0]
        queries = re.findall(
            r'query_prepared\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\s*,',
            function,
        )
        statements = [
            "".join(
                json.loads(part)
                for part in re.findall(r'"(?:\\.|[^"\\])*"', query)
            )
            for query in queries
        ]
        cls.sql = next(
            (sql for sql in statements if sql.startswith("SELECT path FROM virtual_files")),
            None,
        )
        if cls.sql is None:
            raise AssertionError("Cannot locate the production cd query")
        # SQLite 使用 BLOB 表示字节串；其 BINARY 类型不能模拟 MySQL 的 BINARY。
        # 仅适配这处方言差异，仍执行从生产源码提取的查询和参数占位符。
        cls.sqlite_sql = cls.sql.replace(" AS BINARY)", " AS BLOB)")

    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.addCleanup(self.db.close)
        self.db.execute(
            "CREATE TABLE virtual_files ("
            "id INTEGER PRIMARY KEY, username TEXT, parent_id INTEGER, "
            # 使用不区分 ASCII 大小写的列排序，验证查询显式转换后的字节精确比较。
            "file_name TEXT, path TEXT COLLATE NOCASE, file_type TEXT)"
        )
        self.db.executemany(
            "INSERT INTO virtual_files VALUES (?, ?, ?, ?, ?, ?)",
            [
                (1, "alice", -1, "/", "/", "directory"),
                (2, "alice", 1, "docs", "/docs", "directory"),
                (3, "alice", 2, "sub", "/docs/sub", "directory"),
                (4, "alice", 1, "a.txt", "/a.txt", "file"),
                (5, "alice", 1, "archive", "/archive", "directory"),
                (6, "alice", 5, "docs", "/archive/docs", "directory"),
                (7, "bob", -1, "/", "/", "directory"),
                (8, "bob", 7, "docs", "/docs", "directory"),
                (9, "bob", 7, "private", "/private", "directory"),
                (10, "alice", 1, "my folder", "/my folder", "directory"),
            ],
        )

    def lookup(self, target, username="alice"):
        return self.db.execute(self.sqlite_sql, (username, target)).fetchall()

    def test_enters_child_from_root(self):
        self.assertEqual(self.lookup("/docs"), [("/docs",)])

    def test_enters_nested_child(self):
        self.assertEqual(self.lookup("/docs/sub"), [("/docs/sub",)])

    def test_same_name_is_resolved_by_full_path(self):
        self.assertEqual(self.lookup("/archive/docs"), [("/archive/docs",)])
        self.assertEqual(self.lookup("/docs"), [("/docs",)])

    def test_rejects_regular_file(self):
        self.assertEqual(self.lookup("/a.txt"), [])

    def test_rejects_missing_directory(self):
        self.assertEqual(self.lookup("/missing"), [])

    def test_cannot_enter_another_users_directory(self):
        self.assertEqual(self.lookup("/private"), [])
        self.assertEqual(self.lookup("/private", "bob"), [("/private",)])

    def test_directory_name_can_contain_spaces(self):
        self.assertEqual(self.lookup("/my folder"), [("/my folder",)])

    def test_sql_looking_name_is_literal_data(self):
        name = "x' OR 1=1 -- "
        self.assertEqual(self.lookup("/" + name), [])
        self.db.execute(
            "INSERT INTO virtual_files VALUES (?, ?, ?, ?, ?, ?)",
            (11, "alice", 1, name, "/" + name, "directory"),
        )
        self.assertEqual(self.lookup("/" + name), [("/" + name,)])

    def test_root_is_queried_as_an_existing_directory(self):
        self.assertEqual(self.lookup("/"), [("/",)])

    def test_missing_root_returns_no_record(self):
        self.db.execute("DELETE FROM virtual_files WHERE username = ? AND path = ?", ("alice", "/"))
        self.assertEqual(self.lookup("/"), [])

    def test_same_path_is_scoped_to_current_user(self):
        self.assertEqual(self.lookup("/docs", "bob"), [("/docs",)])
        self.assertEqual(self.lookup("/docs", "carol"), [])

    def test_path_comparison_is_case_sensitive(self):
        self.assertEqual(self.lookup("/Docs"), [])
        self.assertEqual(self.lookup("/docs/Sub"), [])

    def test_prefix_is_not_a_complete_path(self):
        self.assertEqual(self.lookup("/doc"), [])
        self.assertEqual(self.lookup("/docs-extra"), [])

    def test_wildcards_are_literal_data(self):
        self.assertEqual(self.lookup("/%"), [])
        self.assertEqual(self.lookup("/doc_"), [])

    def test_trailing_spaces_are_not_trimmed(self):
        self.assertEqual(self.lookup("/my folder "), [])

    def test_directory_type_is_required(self):
        self.db.execute("UPDATE virtual_files SET file_type = 'file' WHERE id = 2")
        self.assertEqual(self.lookup("/docs"), [])

    def test_duplicate_targets_are_not_hidden_by_limit(self):
        # 私有 C++ 方法要求结果恰好一行，因此 SQL 不应通过 LIMIT 隐藏损坏的重复记录。
        self.db.execute(
            "INSERT INTO virtual_files VALUES (?, ?, ?, ?, ?, ?)",
            (11, "alice", 1, "duplicate", "/docs", "directory"),
        )
        self.assertEqual(self.lookup("/docs"), [("/docs",), ("/docs",)])


if __name__ == "__main__":
    unittest.main()
