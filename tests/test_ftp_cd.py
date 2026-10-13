"""Execute the production cd SQL against an isolated in-memory directory tree."""

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
            / "src/ftp/service_engine.cpp"
        ).read_text()
        function = source.split("StorageEngine::move_dir_by_current_dir", 1)[1]
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
            (sql for sql in statements if sql.startswith("SELECT child.path")),
            None,
        )
        cls.parent_sql = next(
            (sql for sql in statements if sql.startswith("SELECT parent.path")),
            None,
        )
        if cls.sql is None:
            raise AssertionError("Cannot locate the production cd query")

    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.addCleanup(self.db.close)
        self.db.execute(
            "CREATE TABLE virtual_files ("
            "id INTEGER PRIMARY KEY, username TEXT, parent_id INTEGER, "
            "file_name TEXT, path TEXT, file_type TEXT)"
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

    def lookup(self, name, current="/", username="alice"):
        return self.db.execute(self.sql, (username, name, current)).fetchall()

    def lookup_parent(self, current, username="alice"):
        self.assertIsNotNone(self.parent_sql, "cd .. lookup is not implemented")
        return self.db.execute(self.parent_sql, (username, current)).fetchall()

    def test_enters_child_from_root(self):
        self.assertEqual(self.lookup("docs"), [("/docs",)])

    def test_enters_nested_child(self):
        self.assertEqual(self.lookup("sub", "/docs"), [("/docs/sub",)])

    def test_same_name_is_resolved_under_current_parent(self):
        self.assertEqual(self.lookup("docs", "/archive"), [("/archive/docs",)])

    def test_rejects_regular_file(self):
        self.assertEqual(self.lookup("a.txt"), [])

    def test_rejects_missing_directory(self):
        self.assertEqual(self.lookup("missing"), [])

    def test_cannot_enter_another_users_directory(self):
        self.assertEqual(self.lookup("private"), [])

    def test_directory_name_can_contain_spaces(self):
        self.assertEqual(self.lookup("my folder"), [("/my folder",)])

    def test_sql_looking_name_is_literal_data(self):
        name = "x' OR 1=1 -- "
        self.assertEqual(self.lookup(name), [])
        self.db.execute(
            "INSERT INTO virtual_files VALUES (?, ?, ?, ?, ?, ?)",
            (11, "alice", 1, name, "/" + name, "directory"),
        )
        self.assertEqual(self.lookup(name), [("/" + name,)])

    def test_returns_from_nested_directory(self):
        self.assertEqual(self.lookup_parent("/docs/sub"), [("/docs",)])

    def test_returns_to_root_from_top_level_directory(self):
        self.assertEqual(self.lookup_parent("/docs"), [("/",)])

    def test_parent_is_resolved_from_directory_record(self):
        self.assertEqual(self.lookup_parent("/archive/docs"), [("/archive",)])

    def test_root_has_no_parent_record(self):
        self.assertEqual(self.lookup_parent("/"), [])

    def test_parent_lookup_rejects_missing_current_directory(self):
        self.assertEqual(self.lookup_parent("/missing"), [])

    def test_parent_lookup_rejects_current_regular_file(self):
        self.assertEqual(self.lookup_parent("/a.txt"), [])

    def test_parent_lookup_is_scoped_to_current_user(self):
        self.db.execute(
            "INSERT INTO virtual_files VALUES (?, ?, ?, ?, ?, ?)",
            (12, "alice", 7, "cross-user", "/cross-user", "directory"),
        )
        self.assertEqual(self.lookup_parent("/cross-user"), [])

    def test_parent_lookup_rejects_regular_file_as_parent(self):
        self.db.execute(
            "INSERT INTO virtual_files VALUES (?, ?, ?, ?, ?, ?)",
            (12, "alice", 4, "invalid-parent", "/invalid-parent", "directory"),
        )
        self.assertEqual(self.lookup_parent("/invalid-parent"), [])

    def test_parent_lookup_rejects_missing_parent(self):
        self.db.execute(
            "INSERT INTO virtual_files VALUES (?, ?, ?, ?, ?, ?)",
            (12, "alice", 999, "orphan", "/orphan", "directory"),
        )
        self.assertEqual(self.lookup_parent("/orphan"), [])


if __name__ == "__main__":
    unittest.main()
