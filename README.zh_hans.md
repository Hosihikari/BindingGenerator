# Hosihikari.Bindings.Generator

[English](README.md) | [简体中文](README.zh_hans.md)

生成器把版本化 IR 转成 C ABI **BindingShim** 与原生风格 C# **Interop**。它不产出 .NET 风格的手写 Idiomatic 层。

## 边界

- **输入**：libTooling 前端产出的 IR 分片，以及 IR 库契约。
- **输出**：生成的 C++（BindingShim）与 C#（Interop）源码。两者提交在绑定仓，并由 CI 重跑比对。
- **不在这里**：手写 Idiomatic 包装、HostingShim、CLR 寄宿。
- IR 库是并列 checkout（`../Ir`）；CI 用 `actions/checkout` 指定 `repository`/`path` 拉取。

## 工程

| 路径 | 用途 |
|---|---|
| `src/Hosihikari.Bindings.Generator` | .NET CLI：version、IR 校验、覆盖校验 |
| `frontend/` | libTooling CMake 目标（LLVM 23.1.3） |
| `tests/fixtures/` | CLI 契约与真实 shim 作业使用的 IR fixture |
| `tests/BindingShimSmoke` | LLAPI 切片的 Windows 端到端 smoke test |
| `tests/RealShimSmoke` | link-carrier fixture 的 CI smoke test |

## 命令

```shell
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- version
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- validate-ir tests/fixtures/basic/ir-manifest.json tests/fixtures/basic
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- emit tests/fixtures/basic/ir-manifest.json tests/fixtures/basic artifacts/emitted
```

`validate-ir` 读取 manifest 与分片目录下所有 `*.jsonl`，然后运行 IR 校验器。`validate-coverage` 对 `coverage/summary.json` + `coverage/manifest.jsonl` 做同样的事。

`emit` 写出 `interop/`（C#）、`shim/`（C++ BindingShim）与 `coverage/`（manifest + summary）。传入 `--native-root <path>` 会为 IR 的源文件发射真实 BindingShim：

```shell
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- emit `
  artifacts/llapi-full-ir/ir-manifest.json artifacts/llapi-full-ir artifacts/llapi-real-shim `
  --native-root C:/path/to/LeviLamina
```

不传 `--native-root` 时写出 fixture CI 使用的契约 stub。

### 真实 BindingShim v1

- C ABI 是每个 carrier surface 一份版本化的 `HosihikariBindingTableV1`。句柄为 `void*`；拥有句柄由构造函数工厂产生，由生成的释放器回收。字段字符串以借用 `HosihikariStringView` 返回，托管侧立即复制；字符串参数为 UTF-8 的 `(const char*, size_t)` 对。
- 投影构造函数、析构函数、公共字段与受支持的方法。构造函数与方法接受多个参数。方法可返回 `void`、基础类型、不透明指针或字符串；字符串结果被复制到 shim 的线程局部缓冲区，托管包装立即复制。每次调用最多固定一个字符串参数。
- 仅投影非模板、public、源文件存在于 native root 的类。默认模式还要求 link-safe：所有方法 inline，所有基类也 link-safe。`--link-carrier` 则链接 carrier import library，并接纳带 `dllexport`/`dllimport` 或 `LLAPI` 类宏的 out-of-line 成员；基类必须能在 IR 中解析。抽象类与含借用字符串构造函数的类不生成工厂。
- 每个表函数都是 C++ function-try-block。原生异常被捕获到由 `hosihikari_get_last_error` 暴露的线程局部消息中，C++ 异常不会跨越托管边界。工厂失败返回空句柄，生成的 `Create` 抛出带该消息的 `InvalidOperationException`。
- `NativeMethods.Load(path)` 在使用前校验导出的表版本与 `sizeof(Table)`；表的前两个字段是 `version` 与 `size`。

`frontend/build-shim.ps1` 用 clang-cl 与 `/MD` 针对 LeviLamina 的 include 环境编译生成的 shim。用 `-AdditionalLinkLibrary` 传入 carrier/包库，用 `-DelayLoadLibrary LeviLamina.dll` 构建可独立加载的 smoke 版本。`frontend/smoke-shim.ps1` 发射、构建并运行 `tests/BindingShimSmoke`；加 `-LinkCarrier` 会用 delay-load 链接 carrier import library。

## 本地 LLAPI 切片

`frontend/run-local.ps1` 解析 `ll/api/event/Event.h`，把 IR 写到 `artifacts/llapi-ir` 并校验；`frontend/run-umbrella.ps1 -Module ll/api` 解析完整的 `src/ll/api` + `src-server/ll/api` 伞头，复用 LeviLamina 的 `compile_commands.json`，当前产出 461 声明 / 1,394 成员。IR 为每个方法记录 `inline`/`defined`，BindingShim 规划器据此判断 link-safe。

```shell
powershell -File frontend/run-umbrella.ps1 -Module ll/api -Output artifacts/llapi-full-ir
powershell -File frontend/smoke-shim.ps1 -IrDirectory artifacts/llapi-full-ir
powershell -File frontend/smoke-shim.ps1 -IrDirectory artifacts/llapi-full-ir -LinkCarrier
```

完整 LLAPI 伞头当前选出 169 个 link-safe 函数；用 `--link-carrier` 链接 `LeviLamina.lib` 时为 426 个函数。切片覆盖 `ll::event::EventId`、`ll::event::EventIdView`、`ll::hash_utils::HashedIdBase`、`ll::hash_utils::HashCombiner`、`ll::data::Version` 以及其他符合条件的类。Smoke test 从托管字符串创建原生 `EventId`，读取名称与哈希，比较两个 `HashedIdBase` 视图，调用 `HashCombiner::hash()`，并让 `ll::data::Version` 经过字符串构造、`to_string()`、字段读取、`valid(string)` 与 `isIdenticalTo(Version)` 往返。其余成员保留 Interop stub，并在覆盖清单中只记录 `interop` 发射器。

## 许可证

GPL-3.0-or-later，见 `LICENSE.md`。
