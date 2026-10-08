#!/bin/bash
# =============================================================================
# C++ 玩具级存储服务 - 构建脚本
# =============================================================================

set -euo pipefail

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印函数
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"

# 默认参数
BUILD_TYPE="Release"
BUILD_TESTS="ON"
CLEAN_BUILD=false
INSTALL=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# 使用帮助
usage() {
    cat << EOF
Usage: $(basename "$0") [OPTIONS]

Options:
    -t, --type TYPE     构建类型: Debug, Release, RelWithDebInfo (默认: Release)
    -j, --jobs N        并行编译任务数 (默认: 自动检测CPU核心数)
    -c, --clean         清理 build/、bin/、lib/ 后重新构建
    -n, --no-tests      不构建或运行测试
    -i, --install       构建后安装
    -h, --help          显示此帮助信息

Examples:
    $(basename "$0")                    # Release 构建
    $(basename "$0") -t Debug           # Debug 构建
    $(basename "$0") -c -t Debug        # 清理后 Debug 构建
    $(basename "$0") -j 8               # 使用8个并行任务
    $(basename "$0") -i                 # 构建并安装
EOF
}

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--type)
            if [[ $# -lt 2 ]]; then
                print_error "$1 需要构建类型"
                exit 1
            fi
            BUILD_TYPE="$2"
            shift 2
            ;;
        -j|--jobs)
            if [[ $# -lt 2 ]]; then
                print_error "$1 需要并行任务数"
                exit 1
            fi
            JOBS="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -n|--no-tests)
            BUILD_TESTS="OFF"
            shift
            ;;
        -i|--install)
            INSTALL=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            print_error "未知选项: $1"
            usage
            exit 1
            ;;
    esac
done

# 验证构建类型
case $BUILD_TYPE in
    Debug|Release|RelWithDebInfo|MinSizeRel)
        ;;
    *)
        print_error "无效的构建类型: $BUILD_TYPE"
        print_info "有效选项: Debug, Release, RelWithDebInfo, MinSizeRel"
        exit 1
        ;;
esac

if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    print_error "并行任务数必须是正整数: $JOBS"
    exit 1
fi

# 打印构建配置
echo ""
echo "==========================================="
echo "   C++ 玩具级存储服务 - 构建脚本"
echo "==========================================="
echo ""
print_info "项目目录: ${PROJECT_ROOT}"
print_info "构建目录: ${BUILD_DIR}"
print_info "构建类型: ${BUILD_TYPE}"
print_info "并行任务: ${JOBS}"
print_info "构建测试: ${BUILD_TESTS}"
print_info "清理构建: ${CLEAN_BUILD}"
print_info "安装: ${INSTALL}"
echo ""

# 检查依赖
print_info "检查依赖..."

check_command() {
    if ! command -v "$1" &> /dev/null; then
        print_error "未找到 $1，请先安装"
        return 1
    fi
    return 0
}

DEPS_OK=true
check_command cmake || DEPS_OK=false
check_command make || DEPS_OK=false
check_command protoc || DEPS_OK=false
check_command grpc_cpp_plugin || DEPS_OK=false

if [ "$DEPS_OK" = false ]; then
    print_error "缺少必要依赖，请先安装"
    echo ""
    print_info "Ubuntu/Debian 安装命令:"
    echo "  sudo apt-get update"
    echo "  sudo apt-get install -y build-essential cmake"
    echo "  sudo apt-get install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc"
    echo "  sudo apt-get install -y libssl-dev libyaml-cpp-dev"
    echo "  sudo apt-get install -y libgtest-dev"
    echo ""
    exit 1
fi

print_success "依赖检查通过"

# 清理所有由 CMake 写入的项目内构建输出
if [ "$CLEAN_BUILD" = true ]; then
    print_info "清理 build/、bin/ 和 lib/ ..."
    cmake -E rm -rf "${BUILD_DIR}" "${PROJECT_ROOT}/bin" "${PROJECT_ROOT}/lib"
    print_success "旧构建结果已清理"
fi

# CMake 配置
print_info "运行 CMake 配置..."
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DBUILD_TESTS="${BUILD_TESTS}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
print_success "CMake 配置完成"

# 编译
print_info "开始编译 (使用 ${JOBS} 个并行任务)..."
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
print_success "编译完成"

# 运行测试 (如果构建了测试)
if [ "$BUILD_TESTS" = "ON" ]; then
    print_info "运行单元测试..."
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
    print_success "所有测试通过"
fi

# 安装
if [ "$INSTALL" = true ]; then
    print_info "安装..."
    cmake --install "${BUILD_DIR}"
    print_success "安装完成"
fi

# 打印结果
echo ""
echo "==========================================="
print_success "构建成功!"
echo "==========================================="
echo ""
print_info "可执行文件位置: ${PROJECT_ROOT}/bin/storage-service"
echo ""

if [ "$INSTALL" = false ]; then
    print_info "运行以下命令启动服务:"
    echo "  ${PROJECT_ROOT}/bin/storage-service --config ${PROJECT_ROOT}/config/config.yaml"
    echo ""
fi
