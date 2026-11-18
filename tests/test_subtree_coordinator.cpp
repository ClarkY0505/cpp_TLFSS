#include "ftp/subtree_coordinator.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using TLSS::FileStorage::AccessMode;
using TLSS::FileStorage::CoordinationRequest;
using TLSS::FileStorage::path_is_within;
using TLSS::FileStorage::PathScope;
using TLSS::FileStorage::PathScopeKind;
using TLSS::FileStorage::requests_conflict;
using TLSS::FileStorage::scopes_overlap;
using TLSS::FileStorage::SubtreeCoordinator;

constexpr auto exact = PathScopeKind::exact;
constexpr auto subtree = PathScopeKind::subtree;

// 此函数的契约要求规范绝对路径。本测试不要求它承担路径归一化、
// 用户身份隔离或非法输入处理；这些属于后续协调器入口的职责。
void expect(const std::string& path, const std::string& root, bool expected) {
  const bool actual = path_is_within(path, root);
  if (actual != expected) {
    throw std::runtime_error("path=" + path + ", root=" + root + ": expected " +
                             (expected ? "true" : "false"));
  }
}

void test_identity() {
  for (const auto& path :
       std::vector<std::string>{"/", "/a", "/docs/a", "/docs/a/sub", "/资料/目录"})
    expect(path, path, true);
}

void test_root() {
  // 根目录本身已带分隔符，不能再要求 path[1] 也是 '/'。
  for (const auto& path :
       std::vector<std::string>{"/", "/a", "/photos", "/docs/a/sub", "/带空格的目录/资料"})
    expect(path, "/", true);
}

void test_descendants() {
  expect("/docs/a/sub", "/docs/a", true);
  expect("/docs/a/sub/deep", "/docs/a", true);
  expect("/a/b/c/d", "/a", true);
  expect("/资料/目录/子目录", "/资料/目录", true);
  expect("/my docs/drafts/sub", "/my docs/drafts", true);
}

void test_boundary() {
  // 字符串前缀相同不代表属于同一棵子树，必须按完整路径分量匹配。
  for (const auto& suffix : std::vector<std::string>{"b", "-backup", "_backup", ".old", "资料"}) {
    expect("/docs/a" + suffix, "/docs/a", false);
    expect("/docs/a" + suffix + "/sub", "/docs/a", false);
  }
  expect("/docs/ab", "/docs/a", false);
  expect("/docs/a/b", "/docs/a", true);
}

void test_unrelated() {
  expect("/docs/a", "/docs/b", false);
  expect("/photos/a", "/docs/a", false);
  expect("/docs", "/docs/a", false);
  expect("/", "/docs/a", false);
  expect("/a", "/longer/path", false);
  // 包含关系有方向，父目录不属于其子目录的子树。
  expect("/docs/a", "/docs/a/sub", false);
  expect("/docs/a/sub", "/docs/a", true);
}

void test_literal_names() {
  // 路径按字节比较，不解释 SQL 通配符，不进行大小写折叠或空格修剪。
  expect("/docs/a_1/sub", "/docs/a_1", true);
  expect("/docs/ab1/sub", "/docs/a_1", false);
  expect("/docs/a%/sub", "/docs/a%", true);
  expect("/docs/anything/sub", "/docs/a%", false);
  expect("/docs/'quoted'/sub", "/docs/'quoted'", true);
  expect("/docs/with spaces/sub", "/docs/with spaces", true);
  expect("/docs/name /sub", "/docs/name", false);
  expect("/docs/name /sub", "/docs/name ", true);
  expect("/Docs/a/sub", "/docs/a", false);
  expect("/docs/A/sub", "/docs/a", false);
  expect("/资料/目录/sub", "/资料/目", false);
}

void test_long_paths() {
  const std::string root = "/" + std::string(1000, 'a');
  expect(root, root, true);
  expect(root + "/child", root, true);
  expect(root + "b/child", root, false);
  expect("/a", root, false);

  // 多层路径验证边界判断不依赖固定目录深度。
  std::string nested;
  for (int index = 0; index < 200; ++index)
    nested += "/d";
  expect(nested + "/leaf", nested, true);
  expect(nested + "x/leaf", nested, false);
  expect(nested, nested + "/leaf", false);
}

void test_component_matrix() {
  struct Sample {
    std::string path;
    std::vector<std::string> components;
  };
  std::vector<Sample> samples{{"/", {}}};
  const std::vector<std::string> names{"a", "ab", "a-b", "资料", "with spaces", "%_"};
  for (const auto& first : names) {
    samples.push_back({"/" + first, {first}});
    for (const auto& second : names)
      samples.push_back({"/" + first + "/" + second, {first, second}});
  }

  // 用独立的“路径分量序列包含”作为预期，不复制被测函数的字符下标算法。
  // 43 个规范路径两两比较，覆盖相同路径、祖先、后代及相似前缀的组合。
  for (const auto& path : samples) {
    for (const auto& root : samples) {
      const bool expected =
          path.components.size() >= root.components.size() &&
          std::equal(root.components.begin(), root.components.end(), path.components.begin());
      expect(path.path, root.path, expected);
    }
  }
}

std::string describe(const PathScope& scope) {
  return scope.path + (scope.kind == exact ? " [exact]" : " [subtree]");
}

void expect_overlap(const PathScope& first, const PathScope& second, bool expected) {
  // 范围重叠是对称关系，交换参数不能改变结果。
  if (scopes_overlap(first, second) != expected || scopes_overlap(second, first) != expected) {
    throw std::runtime_error("first=" + describe(first) + ", second=" + describe(second) +
                             ": expected overlap " + (expected ? "true" : "false"));
  }
}

void test_scope_exact() {
  expect_overlap({"/docs/a", exact}, {"/docs/a", exact}, true);
  expect_overlap({"/docs/a", exact}, {"/docs/a/sub", exact}, false);
  expect_overlap({"/docs/a", exact}, {"/docs/b", exact}, false);
  expect_overlap({"/", exact}, {"/", exact}, true);
  expect_overlap({"/", exact}, {"/docs", exact}, false);
}

void test_scope_mixed() {
  expect_overlap({"/docs/a", subtree}, {"/docs/a", exact}, true);
  expect_overlap({"/docs/a", subtree}, {"/docs/a/sub", exact}, true);
  // 精确父目录不包含后代，不能因为第二个范围是子树就把其祖先包含进来。
  expect_overlap({"/docs/a", exact}, {"/docs/a/sub", subtree}, false);
  expect_overlap({"/docs/a", subtree}, {"/photos", exact}, false);
  expect_overlap({"/", subtree}, {"/photos", exact}, true);
  expect_overlap({"/", exact}, {"/photos", subtree}, false);
}

void test_scope_subtrees() {
  expect_overlap({"/docs/a", subtree}, {"/docs/a", subtree}, true);
  expect_overlap({"/docs/a", subtree}, {"/docs/a/sub/deep", subtree}, true);
  expect_overlap({"/docs/a", subtree}, {"/photos", subtree}, false);
  expect_overlap({"/", subtree}, {"/photos/sub", subtree}, true);
}

void test_scope_boundary() {
  for (const auto& suffix : std::vector<std::string>{"b", "-backup", "_backup", ".old"}) {
    expect_overlap({"/docs/a", subtree}, {"/docs/a" + suffix, exact}, false);
    expect_overlap({"/docs/a", subtree}, {"/docs/a" + suffix + "/sub", subtree}, false);
  }
}

void test_scope_names() {
  expect_overlap({"/资料/目录", subtree}, {"/资料/目录/子目录", exact}, true);
  expect_overlap({"/资料/目", subtree}, {"/资料/目录", subtree}, false);
  expect_overlap({"/docs/a%", subtree}, {"/docs/anything", exact}, false);
  expect_overlap({"/docs/a_", subtree}, {"/docs/ab/sub", subtree}, false);
  expect_overlap({"/docs/name", subtree}, {"/docs/name /sub", exact}, false);
  expect_overlap({"/docs/a", subtree}, {"/docs/A/sub", exact}, false);
  expect_overlap({"/docs/with spaces", exact}, {"/docs/with spaces/sub", subtree}, false);
}

void test_scope_kind_effect() {
  PathScope parent{"/docs/a", exact};
  PathScope child{"/docs/a/sub", exact};
  expect_overlap(parent, child, false);
  parent.kind = subtree;
  expect_overlap(parent, child, true);
  parent.kind = exact;
  child.kind = subtree;
  expect_overlap(parent, child, false);
  parent.kind = subtree;
  expect_overlap(parent, child, true);
}

void test_scope_tree_matrix() {
  struct Directory {
    std::string path;
    int parent;
  };
  const std::vector<Directory> tree{{"/", -1},          {"/docs", 0},    {"/docs/a", 1},
                                    {"/docs/a/sub", 2}, {"/docs/ab", 1}, {"/photos", 0},
                                    {"/photos/x", 5}};
  struct Sample {
    PathScope scope;
    std::vector<int> covered;
  };
  std::vector<Sample> samples;
  for (int root = 0; root < static_cast<int>(tree.size()); ++root) {
    for (const auto kind : {exact, subtree}) {
      Sample sample{{tree[root].path, kind}, {}};
      for (int directory = 0; directory < static_cast<int>(tree.size()); ++directory) {
        // 用父节点关系枚举实际覆盖集合，不调用路径前缀函数计算预期。
        for (int ancestor = directory; ancestor >= 0; ancestor = tree[ancestor].parent) {
          if (ancestor == root) {
            sample.covered.push_back(directory);
            break;
          }
          if (kind == exact)
            break;
        }
      }
      samples.push_back(sample);
    }
  }
  // 14 个范围两两比较：覆盖集合有交集才重叠，涵盖全部四种类型组合。
  for (const auto& first : samples) {
    for (const auto& second : samples) {
      const bool expected =
          std::any_of(first.covered.begin(), first.covered.end(), [&](int directory) {
            return std::find(second.covered.begin(), second.covered.end(), directory) !=
                   second.covered.end();
          });
      expect_overlap(first.scope, second.scope, expected);
    }
  }
}

void expect_conflict(const CoordinationRequest& first, const CoordinationRequest& second,
                     bool expected) {
  // 冲突关系也应对称；新请求与活动请求的比较顺序不应影响是否等待。
  if (requests_conflict(first, second) != expected ||
      requests_conflict(second, first) != expected) {
    throw std::runtime_error("first user=" + first.username + ", second user=" + second.username +
                             ": expected conflict " + (expected ? "true" : "false"));
  }
}

void test_request_users() {
  // 相同路径甚至根子树，属于不同用户时也不应在本协调器中互斥。
  for (const auto first_mode : {AccessMode::read, AccessMode::write}) {
    for (const auto second_mode : {AccessMode::read, AccessMode::write}) {
      expect_conflict({"alice", first_mode, {{"/", subtree}}},
                      {"bob", second_mode, {{"/", subtree}}}, false);
    }
  }
  expect_conflict({"user_1", AccessMode::write, {{"/docs", subtree}}},
                  {"user_10", AccessMode::write, {{"/docs", subtree}}}, false);
}

void test_request_readers() {
  for (const auto first_kind : {exact, subtree}) {
    for (const auto second_kind : {exact, subtree}) {
      expect_conflict({"alice", AccessMode::read, {{"/docs/a", first_kind}}},
                      {"alice", AccessMode::read, {{"/docs/a", second_kind}}}, false);
    }
  }
  expect_conflict({"alice", AccessMode::read, {{"/", subtree}}},
                  {"alice", AccessMode::read, {{"/docs", exact}, {"/photos", subtree}}}, false);
}

void test_request_access_modes() {
  for (const auto first_mode : {AccessMode::read, AccessMode::write}) {
    for (const auto second_mode : {AccessMode::read, AccessMode::write}) {
      const bool expected = first_mode == AccessMode::write || second_mode == AccessMode::write;
      expect_conflict({"alice", first_mode, {{"/docs", exact}}},
                      {"alice", second_mode, {{"/docs", exact}}}, expected);
    }
  }
}

void test_request_scope_kinds() {
  struct Case {
    PathScope first, second;
    bool overlap;
  };
  const std::vector<Case> cases{{{"/docs/a", exact}, {"/docs/a", exact}, true},
                                {{"/docs/a", exact}, {"/docs/a/sub", exact}, false},
                                {{"/docs/a", subtree}, {"/docs/a/sub", exact}, true},
                                {{"/docs/a", exact}, {"/docs/a/sub", subtree}, false},
                                {{"/docs/a", subtree}, {"/docs/a/sub", subtree}, true},
                                {{"/docs/a", subtree}, {"/docs/ab", subtree}, false}};
  for (const auto& item : cases) {
    expect_conflict({"alice", AccessMode::write, {item.first}},
                    {"alice", AccessMode::read, {item.second}}, item.overlap);
  }
}

void test_request_multiple_scopes() {
  // 第一对、交叉两对均不重叠，只有最后一对重叠：必须检查完整个配对集合。
  const CoordinationRequest first{
      "alice", AccessMode::write, {{"/docs", exact}, {"/docs/a", subtree}}};
  const CoordinationRequest second{
      "alice", AccessMode::read, {{"/photos", exact}, {"/docs/a/sub", exact}}};
  expect_conflict(first, second, true);

  auto read_only = first;
  read_only.access = AccessMode::read;
  expect_conflict(read_only, second, false);

  auto another_user = second;
  another_user.username = "bob";
  expect_conflict(first, another_user, false);
}

void test_request_unrelated() {
  expect_conflict({"alice", AccessMode::write, {{"/docs", exact}, {"/docs/a", subtree}}},
                  {"alice", AccessMode::write, {{"/photos", subtree}, {"/docs/ab", subtree}}},
                  false);
  expect_conflict({"alice", AccessMode::write, {{"/docs/a", exact}}},
                  {"alice", AccessMode::read, {{"/docs/a/sub", exact}}}, false);
  expect_conflict({"alice", AccessMode::write, {{"/", exact}}},
                  {"alice", AccessMode::write, {{"/docs", subtree}}}, false);
}

void test_request_directory_delete() {
  const CoordinationRequest deletion{
      "alice", AccessMode::write, {{"/docs", exact}, {"/docs/a", subtree}}};
  // 父目录缓存、目标及任意后代都受影响；其他分支不属于本次删除范围。
  for (const auto& path : std::vector<std::string>{"/docs", "/docs/a", "/docs/a/sub/deep"}) {
    expect_conflict(deletion, {"alice", AccessMode::read, {{path, exact}}}, true);
  }
  for (const auto& path : std::vector<std::string>{"/", "/photos", "/docs/ab", "/docs/other/sub"}) {
    expect_conflict(deletion, {"alice", AccessMode::read, {{path, exact}}}, false);
  }
}

void test_request_permutations() {
  const CoordinationRequest first{
      "alice", AccessMode::write, {{"/photos", exact}, {"/docs", exact}, {"/docs/a", subtree}}};
  const CoordinationRequest second{
      "alice",
      AccessMode::read,
      {{"/docs/ab", exact}, {"/docs/a/sub", exact}, {"/elsewhere", exact}}};
  std::array<std::size_t, 3> first_order{0, 1, 2};
  do {
    std::array<std::size_t, 3> second_order{0, 1, 2};
    do {
      auto permuted_first = first;
      auto permuted_second = second;
      for (std::size_t index = 0; index < 3; ++index) {
        permuted_first.scopes[index] = first.scopes[first_order[index]];
        permuted_second.scopes[index] = second.scopes[second_order[index]];
      }
      // 两边共 36 种顺序组合，同一组范围的冲突结果必须保持不变。
      expect_conflict(permuted_first, permuted_second, true);
    } while (std::next_permutation(second_order.begin(), second_order.end()));
  } while (std::next_permutation(first_order.begin(), first_order.end()));
}

void expect_rejected(const CoordinationRequest& request) {
  SubtreeCoordinator coordinator;
  bool rejected = false;
  try {
    auto guard = coordinator.acquire(request);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  if (!rejected)
    throw std::runtime_error("invalid coordination request was accepted");
}

void reject_paths(const std::vector<std::string>& paths) {
  for (const auto& path : paths) {
    for (const auto kind : {exact, subtree}) {
      expect_rejected({"alice", AccessMode::write, {{path, kind}}});
    }
  }
}

void test_validation_empty_request() {
  expect_rejected({"", AccessMode::read, {{"/docs", exact}}});
  expect_rejected({"alice", AccessMode::write, {}});
  expect_rejected({"", AccessMode::read, {}});
}

void test_validation_access_mode() {
  expect_rejected({"alice", static_cast<AccessMode>(99), {{"/docs", exact}}});
}

void test_validation_scope_kind() {
  expect_rejected({"alice", AccessMode::write, {{"/docs", static_cast<PathScopeKind>(99)}}});
}

void test_validation_absolute_paths() {
  reject_paths({"", "docs/a", "./docs", "../docs"});
}

void test_validation_canonical_paths() {
  // 不接受多个 '/'、点分量或末尾分隔符；协调器不静默修改调用者的路径。
  reject_paths({"/docs//a", "//docs/a", "///", "/docs/./a", "/docs/a/../b", "/docs/.", "/docs/..",
                "/docs/a/", "//"});
}

void test_validation_component_names() {
  reject_paths({"/docs/a\\b", std::string("/docs/a\0b", 9), "/docs/a\nb",
                "/docs/" + std::string(1, '\x7f'), "/docs/" + std::string(256, 'x')});
}

void test_validation_path_limits() {
  const std::string component(255, 'x');
  const auto prefix = "/" + component + "/" + component + "/" + component;
  // 所有分量均不超过 255 字节，但整个路径为 1025 字节，必须拒绝。
  reject_paths({prefix + "/" + std::string(254, 'y') + "/z"});
}

void test_validation_later_scope() {
  // 必须检查整组范围，不能只校验第一项；同一个请求尚未入场时整体拒绝。
  for (const auto& path : std::vector<std::string>{"relative", "/docs//a", "/docs/a/"}) {
    expect_rejected({"alice", AccessMode::write, {{"/photos", exact}, {path, subtree}}});
  }
  expect_rejected({"alice",
                   AccessMode::write,
                   {{"/photos", exact}, {"/docs", static_cast<PathScopeKind>(99)}}});
}

void test_validation_valid_requests() {
  std::vector<std::string> paths{"/",           "/docs/a",    "/docs/with spaces",
                                 "/docs/name ", "/资料/目录", "/docs/%_ 'quoted'",
                                 "/docs/..."};
  const std::string component(255, 'x');
  paths.push_back("/" + component + "/" + component + "/" + component + "/" + component);
  // 精确 1024 字节、255 字节分量，以及合法特殊名字均可使用。
  for (const auto& path : paths) {
    for (const auto mode : {AccessMode::read, AccessMode::write}) {
      for (const auto kind : {exact, subtree}) {
        SubtreeCoordinator coordinator;
        auto guard = coordinator.acquire({"alice", mode, {{path, kind}}});
        guard.reset();
        guard.reset();  // 重复释放必须无副作用。
      }
    }
  }
}

void run(const std::string& scenario) {
  if (scenario == "identity")
    test_identity();
  else if (scenario == "root")
    test_root();
  else if (scenario == "descendants")
    test_descendants();
  else if (scenario == "boundary")
    test_boundary();
  else if (scenario == "unrelated")
    test_unrelated();
  else if (scenario == "literal_names")
    test_literal_names();
  else if (scenario == "long_paths")
    test_long_paths();
  else if (scenario == "component_matrix")
    test_component_matrix();
  else if (scenario == "scope_exact")
    test_scope_exact();
  else if (scenario == "scope_mixed")
    test_scope_mixed();
  else if (scenario == "scope_subtrees")
    test_scope_subtrees();
  else if (scenario == "scope_boundary")
    test_scope_boundary();
  else if (scenario == "scope_literal_names")
    test_scope_names();
  else if (scenario == "scope_kind_effect")
    test_scope_kind_effect();
  else if (scenario == "scope_tree_matrix")
    test_scope_tree_matrix();
  else if (scenario == "request_users")
    test_request_users();
  else if (scenario == "request_readers")
    test_request_readers();
  else if (scenario == "request_access_modes")
    test_request_access_modes();
  else if (scenario == "request_scope_kinds")
    test_request_scope_kinds();
  else if (scenario == "request_multiple_scopes")
    test_request_multiple_scopes();
  else if (scenario == "request_unrelated")
    test_request_unrelated();
  else if (scenario == "request_directory_delete")
    test_request_directory_delete();
  else if (scenario == "request_permutations")
    test_request_permutations();
  else if (scenario == "validation_empty_request")
    test_validation_empty_request();
  else if (scenario == "validation_access_mode")
    test_validation_access_mode();
  else if (scenario == "validation_scope_kind")
    test_validation_scope_kind();
  else if (scenario == "validation_absolute_paths")
    test_validation_absolute_paths();
  else if (scenario == "validation_canonical_paths")
    test_validation_canonical_paths();
  else if (scenario == "validation_component_names")
    test_validation_component_names();
  else if (scenario == "validation_path_limits")
    test_validation_path_limits();
  else if (scenario == "validation_later_scope")
    test_validation_later_scope();
  else if (scenario == "validation_valid_requests")
    test_validation_valid_requests();
  else
    throw std::invalid_argument("unknown subtree path scenario: " + scenario);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2)
      throw std::invalid_argument("expected one subtree path scenario");
    run(argv[1]);
    std::cout << "PASS subtree coordinator " << argv[1] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
