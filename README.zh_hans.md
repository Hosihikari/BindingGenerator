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
| `tests/fixtures/` | CLI 契约作业使用的最小 IR fixture |

## 命令

```shell
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- version
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- validate-ir tests/fixtures/basic/ir-manifest.json tests/fixtures/basic
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- emit tests/fixtures/basic/ir-manifest.json tests/fixtures/basic artifacts/emitted
```

`validate-ir` 读取 manifest 与分片目录下所有 `*.jsonl`，然后运行 IR 校验器。`validate-coverage` 对 `coverage/summary.json` + `coverage/manifest.jsonl` 做同样的事。

`emit` 写出 `interop/`（C# stub）、`shim/`（C++ 函数表 stub）与 `coverage/`（manifest + summary）。stub 固定 IR→产物的契约；BindingShim 调用尚未接通。

## 许可证

GPL-3.0-or-later，见 `LICENSE.md`。
