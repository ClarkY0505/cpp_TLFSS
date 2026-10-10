创建用户根目录
INSERT INTO virtual_files (
    file_name,
    username,
    parent_id,
    path,
    file_type
)
SELECT
    '/',
    'alice',
    -1,
    '/',
    'directory'
FROM virtual_files
WHERE username = 'alice'
  AND path = '/'
  AND parent_id = -1;

为用户 alice 创建一个 /documents 虚拟文件夹，其父目录是 /。
通过 SELECT 获取根目录 / 的真实 id，作为 /documents 的 parent_id。

INSERT INTO virtual_files (
    file_name,
    username,
    parent_id,
    path,
    file_type
)
SELECT
    'documents',
    'alice',
    id,
    '/documents',
    'directory'
FROM virtual_files
WHERE username = 'alice'
  AND path = '/'
  AND parent_id = -1;
