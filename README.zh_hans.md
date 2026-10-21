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
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- emit tests/fixtures/basic/ir-manifest.json tests/fixtures/basic artifacts/emitted --abi-header ../Abi/native/hosihikari_abi.h
```

`validate-ir` 读取 manifest 与分片目录下所有 `*.jsonl`，然后运行 IR 校验器。`validate-coverage` 对 `coverage/summary.json` + `coverage/manifest.jsonl` 做同样的事。

`emit` 写出 `interop/`（C#）、`shim/`（C++ BindingShim）与 `coverage/`（manifest + summary）。传入 `--native-root <path>` 会为 IR 的源文件发射真实 BindingShim：

```shell
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- emit `
  artifacts/llapi-full-ir/ir-manifest.json artifacts/llapi-full-ir artifacts/llapi-real-shim `
  --native-root C:/path/to/LeviLamina --abi-header ../Abi/native/hosihikari_abi.h --link-carrier --ll-allocator
```

`--abi-header` 指向 `Hosihikari/Bindings.Abi` 的 `native/hosihikari_abi.h`；发射器把它复制到每个 shim 目录，使 shim 构建保持自包含。生成的 C# 引用 `Hosihikari.Bindings.Abi` 提供的 `StringView`、`ArrayView`、`SharedHandle` 与负责版本校验的 `ShimLibrary` 加载器。

不传 `--native-root` 时写出 fixture CI 使用的契约 stub。

### 真实 BindingShim v1

- C ABI 是每个 carrier surface 一份版本化的 `HosihikariBindingTableV1`。句柄为 `void*`；拥有句柄由构造函数工厂产生，由生成的释放器回收。函数表字段使用函数身份的稳定命名（如 `hs_method_1a2b3c4d`），而不是槽位序号；序号仅保留为物理数组位置，因此新增函数不会重命名既有调用点。字段字符串以借用 `HosihikariStringView` 返回，托管侧立即复制；字符串参数为 UTF-8 的 `(const char*, size_t)` 对。
- 投影构造函数、析构函数、公共字段与受支持的方法。生成器有意拒绝的成员会发射为带 `[Obsolete]` 标记的 `NotSupportedException` stub，标记消息包含同样的原因；调用时产生编译器警告 CS0618 并在 IDE 中显示删除线，项目可用 `<WarningsAsErrors>CS0618</WarningsAsErrors>` 将其升级为错误。构造函数与方法接受多个参数。构造函数可以接受指向有生成包装的公有声明的可变引用与右值引用，shim 通过解引用地址传入这些参数。方法可返回 `void`、基础类型、枚举、不透明指针、字符串或符合条件的按值 record；字符串结果被复制到 shim 的线程局部缓冲区，托管包装立即复制。每次调用可以固定多个字符串参数。枚举使用底层整数 ABI。按值 record 参数由调用方句柄解引用传入；按值 record 结果复制为 shim 持有、托管包装可释放的对象。record 与容器字段以借用地址视图暴露；数组字段只生成 getter。`std::vector<T>` 结果通过借用 `HosihikariArrayView` 返回 `T[]`/`string[]`/`nint[]`，包括指针元素；vector 参数展开为隐藏的 `(元素指针, 数量)` 对，托管侧以 `T[]`/`string[]`/`Variant[]` 传入，支持 record 与 variant 元素，每次调用支持多个 vector 参数，每个 vector 使用独立固定的元素视图；`std::initializer_list<T>` 参数仍不投影，因为 C++ 无法构造运行时长度的 initializer list，相关类已有等价的 vector 重载；`std::vector<std::pair<string, uint64>>` 参数以 `KeyValuePair<string, ulong>[]` 传入；`std::optional<T>` 参数与结果通过隐藏的存在标志传递，托管侧使用可空标量、`string?` 或拥有的 `T?`；`std::unique_ptr<T>` 结果以释放所有权的方式返回拥有的 `T` 句柄；`unique_ptr` 参数从包装接收所有权，包装在调用前释放句柄，重复使用会抛异常；带 public 虚析构的抽象接口在此路径下也会生成释放器。`std::shared_ptr<T>` 结果返回 `SharedHandle`，持有一次共享引用并在释放时归还；`shared_ptr`/`weak_ptr` 参数接受 `SharedHandle`，shim 通过带类型校验的 holder 恢复类型化引用。`optional_ref<T>` 参数与结果复用 optional 存在标志，传递借用引用；托管侧为 `T?`/`nint?`。`nonstd::expected_lite::expected<T, E>` 结果返回 `T`，失败时抛出携带原生错误消息的 `InvalidOperationException`；`E` 可为 `ll::Error` 或字符串。`std::variant<T...>` 在 alternative 为字符串、标量/枚举、指针或可复制 record 时生成拥有包装，提供各 alternative 的 `Create`/`As`/`Is`、`Index` 与 `Dispose`；by-value 参数复制，`const&` 参数借用，by-value 结果复制为拥有句柄。成员全部继承（没有字段、构造函数或析构记录）的公有可复制类也可作为 record alternative；生成器会补齐其 `Dispose` 所需的删除函数。`endstone::NotNull<T>`/`Nullable<T>` 返回使用 `SharedHandle`；序列返回值区分借用指针元素（`nint[]`）、拥有元素（record/`unique_ptr` 为 `T[]`/`T?[]`）与共享元素（`shared_ptr`/`NotNull`/`Nullable` 为 `SharedHandle[]`）；`unordered_set<T*>` 返回借用地址数组。`std::chrono::time_point<std::chrono::system_clock>` 映射为 Unix 毫秒；其他 clock 映射为原生 tick 计数；`std::chrono::duration` 映射为原生 count；optional 时间值使用 `long?`。返回的 `std::function<R(Args...)>` 生成带 `Invoke`/`Dispose` 的可调用包装。variant、可调用、chrono、`typeid_t`、`strong_ordering` 与 filesystem path 的引用返回值也按值投影复制，而不是退化为不透明句柄；指向这些类型的指针仍为借用句柄。`Bedrock::typeid_t<C>` 映射为 `ushort`，`std::strong_ordering` 映射为 `sbyte`（-1/0/1），`std::filesystem::path` 映射为 UTF-8 字符串；optional 形式沿用标量/字符串规则。`std::unordered_map<K, V>` 参数与结果使用托管 `Dictionary<K, V>`；键支持字符串/标量/指针，值支持字符串/标量/指针、record 复制与 variant 复制。条目类型无法投影时，引用与指针结果仍返回借用句柄。
- 默认 link-safe 模式投影 public、非模板、源文件存在于 native root、所有方法 inline 且基类 link-safe 的类。`--link-carrier` 另外接纳 public 的具体模板实例，并链接 carrier import library、fmt 与 prelink 生成的 Bedrock 导入。模板实参必须可访问；不完整实参只对标准库或受支持的第三方类型放行。成员模板、右值限定成员与变量模板不投影；模板析构函数需有可见定义才生成释放器。静态方法与静态字段不传 `self`。抽象类不生成工厂。持有 `string_view` 字段的类使用 holder 支撑的工厂：shim 把字符串参数复制到 holder，再基于这些副本构造对象，视图在包装释放前保持有效。
- 每个表函数都是 C++ function-try-block。原生异常被捕获到由 `hosihikari_get_last_error` 暴露的线程局部消息中，C++ 异常不会跨越托管边界。工厂失败返回空句柄，生成的 `Create` 抛出带该消息的 `InvalidOperationException`。
- `std::function<R(Args...)>`、`brstd::move_only_function<R(Args...)>` 与 `brstd::function_ref<R(Args...)>` 参数暴露为 `(invoke, userData, release)` 三元组。shim 用共享回调状态包装成对应的可调用类型（`std::function`、`brstd::move_only_function` 或 `brstd::function_ref`）；最后一个副本销毁时调用 `release(userData)`。v1 支持基础类型/枚举返回，参数支持基础类型、枚举、字符串、指针、引用与按值 record；`const` 标量、枚举与字符串引用按值传递。
- `--ll-allocator` 把 shim 的全局 `operator new`/`delete` 交给 `ll::memory::getDefaultAllocator()`。LeviLamina 等替换全局分配器的 carrier 需要此开关；mock carrier fixture 不启用。
- `NativeMethods.Load(path)` 在使用前校验导出的表版本与 `sizeof(Table)`；表的前两个字段是 `version` 与 `size`。

`frontend/build-shim.ps1` 用 clang-cl 与 `/MD` 针对 LeviLamina 的 include 环境编译生成的 shim。用 `-AdditionalLinkLibrary` 传入 carrier/包库，用 `-DelayLoadLibrary LeviLamina.dll` 构建可独立加载的 smoke 版本。`frontend/build-carrier-shim.ps1` 先编译对象，运行 `prelink` 生成 Bedrock runtime 导入库，再链接 carrier shim。`frontend/smoke-shim.ps1` 发射、构建并运行 `tests/BindingShimSmoke`；加 `-LinkCarrier` 会使用 carrier 构建与 delay-load。

## 本地 LLAPI 切片

`frontend/run-local.ps1` 解析 `ll/api/event/Event.h`，把 IR 写到 `artifacts/llapi-ir` 并校验；`frontend/run-umbrella.ps1 -Module ll/api` 解析完整的 `src/ll/api` + `src-server/ll/api` 伞头，复用 LeviLamina 的 `compile_commands.json`，当前在 IR 1.2.0 产出 1,530 声明 / 6,145 成员。被过滤签名按值引用的公有 record 类型即使头文件位于模块过滤之外，也会连同构造、析构与字段一起写入 IR。IR 为每个方法记录 `inline`/`defined`/`consteval`，并记录成员模板、引用限定符、枚举底层类型与类特性（`abstract`、`copyConstructible`），BindingShim 规划器据此判断资格与所有权。

```shell
powershell -File frontend/run-umbrella.ps1 -Module ll/api -Output artifacts/llapi-full-ir
powershell -File frontend/smoke-shim.ps1 -IrDirectory artifacts/llapi-full-ir
powershell -File frontend/smoke-shim.ps1 -IrDirectory artifacts/llapi-full-ir -LinkCarrier
```

完整 LLAPI 伞头当前选出 214 个 link-safe 函数；用 `--link-carrier` 链接 `LeviLamina.lib` 时为 3,219 个函数。切片覆盖 `ll::event::EventId`、`ll::event::EventIdView`、`ll::hash_utils::HashedIdBase`、`ll::hash_utils::HashCombiner`、`ll::data::Version` 以及其他符合条件的类。Smoke test 从托管字符串创建原生 `EventId`，读取名称与哈希，比较两个 `HashedIdBase` 视图，调用 `HashCombiner::hash()`，并让 `ll::data::Version` 经过字符串构造、`to_string()`、字段读取、`valid(string)` 与 `isIdenticalTo(Version)` 往返。其余成员保留 Interop stub，并在覆盖清单中只记录 `interop` 发射器。

## Endstone 表面

`frontend/run-endstone.ps1` 解析 Endstone 公共伞头（`include/endstone/endstone.hpp`）并提供 expected-lite 与 nlohmann_json 头。`--virtual-dispatch` 为 header-only 虚接口发射免链接 shim：inline 成员与虚方法经宿主提供的 vtable 调用，非虚 out-of-line 成员保留 stub。`frontend/build-endstone-shim.ps1` 用 Endstone 头编译生成的表。Endstone 0.11.14 表面当前选出 1,838 个函数，包含符合条件的模板实例与 inline/constexpr 静态字段。

## 相关仓库

- `Hosihikari/Bindings.Ir`：schema、模型、读写器与校验器。
- `Hosihikari/Bindings.Abi`：共享 ABI 值类型、加载器与原生头文件。
- `Hosihikari/Bindings.Endstone`：Endstone 表面。
- `Hosihikari/Bindings.LeviLamina`：LeviLamina 表面。

## 许可证

GPL-3.0-or-later，见 `LICENSE.md`。
