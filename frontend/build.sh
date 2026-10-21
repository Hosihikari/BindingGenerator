#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build.sh --llvm-root <dir> [options]

Options:
  --llvm-root <dir>   LLVM/Clang 23 install tree containing lib/cmake/clang (required).
  --build-dir <dir>   CMake build directory. Default: build/frontend-linux.
  -h, --help          Show this help.
EOF
}

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_root/.." && pwd)"
llvm_root=""
build_dir="$repo_root/build/frontend-linux"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --llvm-root)
            llvm_root="${2:-}"
            shift 2
            ;;
        --build-dir)
            build_dir="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$llvm_root" ]; then
    usage >&2
    exit 2
fi
llvm_root="$(cd "$llvm_root" && pwd)"
build_dir="$(mkdir -p "$build_dir" && cd "$build_dir" && pwd)"

if [ ! -f "$llvm_root/lib/cmake/clang/ClangConfig.cmake" ] \
    && [ ! -f "$llvm_root/lib64/cmake/clang/ClangConfig.cmake" ]; then
    echo "ClangConfig.cmake was not found under $llvm_root." >&2
    exit 1
fi

generator=()
if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
fi

cmake -S "$script_root" -B "$build_dir" "${generator[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$llvm_root"

cmake --build "$build_dir" --target hosihikari-ir-frontend --parallel

echo "Built $build_dir/hosihikari-ir-frontend"
