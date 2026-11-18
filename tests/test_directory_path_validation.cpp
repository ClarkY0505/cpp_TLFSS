#include "ftp/transfer_validation.h"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(const std::string& path, bool expected) {
  if (TLSS::FileStorage::Transfer::valid_directory_path(path) != expected) {
    // 不直接输出路径，避免控制字符干扰测试日志。
    throw std::runtime_error("unexpected directory path validation result; bytes=" +
                             std::to_string(path.size()));
  }
}

void expect_resolved(const std::string& current, const std::string& argument,
                     const std::optional<std::string>& expected) {
  const auto result = TLSS::FileStorage::Transfer::resolve_directory_path(current, argument);
  if (result != expected) {
    // 不输出原始路径，避免 NUL、换行等非法输入污染日志。
    throw std::runtime_error("unexpected resolved directory path; current bytes=" +
                             std::to_string(current.size()) + ", argument bytes=" +
                             std::to_string(argument.size()));
  }
  if (result && !TLSS::FileStorage::Transfer::valid_directory_path(*result)) {
    throw std::runtime_error("resolver returned a noncanonical directory path");
  }
}

void run_resolver(const std::string& scenario) {
  if (scenario == "absolute") {
    // 绝对路径从虚拟根目录开始，不应拼接或保留当前目录。
    expect_resolved("/docs/a", "/", "/");
    expect_resolved("/docs/a", "/archive", "/archive");
    expect_resolved("/docs/a", "/archive/sub", "/archive/sub");
  } else if (scenario == "relative") {
    expect_resolved("/", "a", "/a");
    expect_resolved("/docs", "a", "/docs/a");
    expect_resolved("/docs/a", "b/c", "/docs/a/b/c");
  } else if (scenario == "dot") {
    expect_resolved("/", ".", "/");
    expect_resolved("/docs/a", ".", "/docs/a");
    expect_resolved("/docs", "./a/./b/.", "/docs/a/b");
    expect_resolved("/docs", "/./archive/.", "/archive");
  } else if (scenario == "parent") {
    // .. 只弹出一个完整分量，并能与后续普通分量组合。
    expect_resolved("/docs/a", "..", "/docs");
    expect_resolved("/docs/a", "../b", "/docs/b");
    expect_resolved("/docs/a", "../../archive", "/archive");
    expect_resolved("/docs", "a/b/../c", "/docs/a/c");
    expect_resolved("/docs", "/a/../b", "/b");
  } else if (scenario == "root_boundary") {
    // 空栈表示虚拟根目录；继续返回上级不能下溢或逃出该根目录。
    expect_resolved("/", "..", "/");
    expect_resolved("/", "../../..", "/");
    expect_resolved("/docs", "../../../a", "/a");
    expect_resolved("/docs", "/../../a/..", "/");
  } else if (scenario == "invalid_current") {
    // 当前目录也必须规范，即使参数为绝对路径也不能跳过前置检查。
    for (const auto& current : std::vector<std::string>{
             "", "docs", "/docs/", "//docs", "/docs//a", "/docs/..",
             "/" + std::string(256, 'x'), std::string("/a\0b", 4)}) {
      expect_resolved(current, "a", std::nullopt);
      expect_resolved(current, "/archive", std::nullopt);
    }
  } else if (scenario == "invalid_argument") {
    // 只归一化 . 和 ..；保留原有重复斜杠、末尾斜杠及普通名称的拒绝规则。
    for (const auto& argument : std::vector<std::string>{
             "", "//", "//archive", "a/", "/archive/", "./", "../", "a//b", "/a//b",
             "a\\b", "a\\b/../valid", std::string(256, 'x'), std::string(256, 'x') + "/.."})
      expect_resolved("/docs", argument, std::nullopt);
    // 不能因为后续 .. 抵消非法分量，就允许控制字符通过。
    for (int byte = 0; byte <= 32; ++byte) {
      const std::string invalid = "a" + std::string(1, static_cast<char>(byte == 32 ? 127 : byte)) + "b";
      expect_resolved("/docs", invalid, std::nullopt);
      expect_resolved("/docs", invalid + "/../valid", std::nullopt);
    }
  } else if (scenario == "limits") {
    const std::string prefix = "/" + std::string(255, 'a') + "/" + std::string(255, 'b') +
                               "/" + std::string(255, 'c');
    const std::string maximum = prefix + "/" + std::string(255, 'd');
    const std::string oversized = prefix + "/" + std::string(254, 'd') + "/x";
    expect_resolved("/", maximum, maximum);  // 1024 字节的规范结果合法。
    expect_resolved("/", oversized, std::nullopt);
    expect_resolved(maximum, ".", maximum);
    expect_resolved(maximum, "x", std::nullopt);
    expect_resolved(maximum, "../" + std::string(255, 'e'), prefix + "/" + std::string(255, 'e'));
    expect_resolved("/docs", std::string(255, 'x'), "/docs/" + std::string(255, 'x'));
    // 1024 字节限制作用于最终规范路径，先返回上级再检查结果长度。
    expect_resolved("/", maximum + "/..", prefix);
    expect_resolved(oversized, "/", std::nullopt);
  } else if (scenario == "literal_names") {
    // 普通名称按字节保留：不裁剪空格，不将 ... 当作 ..，不解释 SQL 通配符。
    expect_resolved("/资料/my docs", "../目录", "/资料/目录");
    expect_resolved("/docs", ".../.config/a%_/'quoted'/name ", "/docs/.../.config/a%_/'quoted'/name ");
    expect_resolved("/docs", "-r/--", "/docs/-r/--");
  } else {
    throw std::runtime_error("unknown resolver scenario");
  }
}

void run(const std::string& scenario) {
  if (scenario.rfind("resolve_", 0) == 0) {
    run_resolver(scenario.substr(8));
    return;
  }
  if (scenario == "valid") {
    // 根目录、单层、多层及包含空格/中文的合法名称都沿用项目原有规则。
    for (const auto& path :
         std::vector<std::string>{"/", "/archive", "/archive/docs", "/带空格的目录/my docs",
                                  "/-r/--", "/docs/a%_/'quoted'", "/docs/name "}) {
      expect(path, true);
    }
  } else if (scenario == "relative") {
    for (const auto& path :
         std::vector<std::string>{"", "archive", "archive/docs", "./docs", "../docs"})
      expect(path, false);
  } else if (scenario == "separators") {
    // 不进行路径归一化：重复分隔符、末尾斜杠和反斜杠必须直接拒绝。
    for (const auto& path : std::vector<std::string>{"//", "//docs", "/archive/", "/archive//docs",
                                                     "/archive///docs", "/a/b/", "/a\\b"})
      expect(path, false);
  } else if (scenario == "dot_components") {
    for (const auto& path : std::vector<std::string>{"/.", "/..", "/./docs", "/../docs", "/docs/.",
                                                     "/docs/..", "/a/../b", "/a/./b"})
      expect(path, false);
    // 点号出现在普通名称中合法，不能通过字符串包含判断拒绝所有点号。
    expect("/.config/archive.old/...", true);
  } else if (scenario == "component_limits") {
    expect("/" + std::string(255, 'a'), true);
    expect("/" + std::string(256, 'a'), false);
    expect("/docs/" + std::string(255, 'b'), true);
    expect("/docs/" + std::string(256, 'b'), false);
    expect("/" + std::string(256, 'a') + "/docs", false);
  } else if (scenario == "path_limits") {
    // 两侧每个分量均不超长，单独验证完整路径的 1024 字节边界。
    const std::string prefix =
        "/" + std::string(255, 'a') + "/" + std::string(255, 'b') + "/" + std::string(255, 'c');
    expect(prefix + "/" + std::string(255, 'd'), true);          // 1024 字节。
    expect(prefix + "/" + std::string(254, 'd') + "/x", false);  // 1025 字节。
  } else if (scenario == "control_bytes") {
    // 使用带长度的 std::string 保留 NUL，覆盖每一种禁止的控制字节。
    for (int byte = 0; byte <= 32; ++byte) {
      const char ch = static_cast<char>(byte == 32 ? 127 : byte);
      expect("/docs/a" + std::string(1, ch) + "b", false);
      expect("/a" + std::string(1, ch) + "b/docs", false);
    }
  } else {
    throw std::runtime_error("unknown test scenario");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected one scenario");
    run(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
