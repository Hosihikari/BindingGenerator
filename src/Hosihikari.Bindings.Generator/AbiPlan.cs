using Hosihikari.Bindings.Ir;

namespace Hosihikari.Bindings.Generator;

internal enum AbiKind
{
    Void,
    Bool,
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    USize,
    ISize,
    F32,
    F64,
    Opaque,
    StringView,
}

internal enum AbiFunctionKind
{
    Create,
    Destroy,
    Get,
    Set,
    Method,
}

internal sealed class AbiParameter
{
    public string Name { get; init; } = "";
    public AbiKind Kind { get; init; }
    public string NativeType { get; init; } = "";
    public string? NativePointee { get; init; }
    public bool IsReference { get; init; }
    public bool IsRValueReference { get; init; }
    public bool IsConst { get; init; }
    public string? WrapperType { get; init; }
}

internal sealed class AbiFunction
{
    public int Index { get; set; }
    public string TableField => "f" + Index.ToString(System.Globalization.CultureInfo.InvariantCulture);
    public string CppName { get; init; } = "";
    public AbiFunctionKind Kind { get; init; }
    public string Surface { get; init; } = "";
    public string DeclarationId { get; init; } = "";
    public string? MemberId { get; init; }
    public string ClassCppName { get; init; } = "";
    public DeclarationName ClassName { get; init; }
    public string NativeMemberName { get; init; } = "";
    public string CSharpMemberName { get; init; } = "";
    public AbiKind ReturnKind { get; init; }
    public string NativeReturnType { get; init; } = "";
    public string? ReturnWrapperType { get; init; }
    public bool ReturnOwned { get; init; }
    public List<AbiParameter> Parameters { get; init; } = [];
    public bool Static { get; init; }
}

internal sealed class AbiPlan
{
    private AbiPlan(
        IReadOnlyList<AbiFunction> functions,
        IReadOnlyDictionary<string, DeclarationName> declarationNames)
    {
        Functions = functions;
        DeclarationNames = declarationNames;
        CreateByMember = functions
            .Where(function => function.Kind == AbiFunctionKind.Create && function.MemberId is not null)
            .ToDictionary(function => function.MemberId!, StringComparer.Ordinal);
        GetByMember = functions
            .Where(function => function.Kind == AbiFunctionKind.Get && function.MemberId is not null)
            .ToDictionary(function => function.MemberId!, StringComparer.Ordinal);
        SetByMember = functions
            .Where(function => function.Kind == AbiFunctionKind.Set && function.MemberId is not null)
            .ToDictionary(function => function.MemberId!, StringComparer.Ordinal);
        MethodByMember = functions
            .Where(function => function.Kind == AbiFunctionKind.Method && function.MemberId is not null)
            .ToDictionary(function => function.MemberId!, StringComparer.Ordinal);
        DestroyByDeclaration = functions
            .Where(function => function.Kind == AbiFunctionKind.Destroy)
            .ToDictionary(function => function.DeclarationId, StringComparer.Ordinal);
        FunctionsByDeclaration = functions
            .GroupBy(function => function.DeclarationId, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.ToList(), StringComparer.Ordinal);
    }

    public IReadOnlyList<AbiFunction> Functions { get; }
    public IReadOnlyDictionary<string, DeclarationName> DeclarationNames { get; }
    public IReadOnlyDictionary<string, AbiFunction> CreateByMember { get; }
    public IReadOnlyDictionary<string, AbiFunction> GetByMember { get; }
    public IReadOnlyDictionary<string, AbiFunction> SetByMember { get; }
    public IReadOnlyDictionary<string, AbiFunction> MethodByMember { get; }
    public IReadOnlyDictionary<string, AbiFunction> DestroyByDeclaration { get; }
    public IReadOnlyDictionary<string, List<AbiFunction>> FunctionsByDeclaration { get; }

    public static AbiPlan Empty { get; } = new([], new Dictionary<string, DeclarationName>(StringComparer.Ordinal));

    public IEnumerable<AbiFunction> FunctionsForSurface(string surface) =>
        Functions.Where(function => string.Equals(function.Surface, surface, StringComparison.Ordinal));

    public bool HasMemberFunction(string memberId) =>
        CreateByMember.ContainsKey(memberId)
        || GetByMember.ContainsKey(memberId)
        || SetByMember.ContainsKey(memberId)
        || MethodByMember.ContainsKey(memberId);

    public static AbiPlan Build(IReadOnlyList<IrRecord> records, string? nativeRoot, bool linkCarrier = false)
    {
        if (string.IsNullOrWhiteSpace(nativeRoot) || !Directory.Exists(nativeRoot))
        {
            return Empty;
        }

        var allClassDeclarations = records.OfType<DeclarationRecord>()
            .Where(declaration => declaration.Kind is "class" or "struct" or "union")
            .OrderBy(declaration => declaration.Id, StringComparer.Ordinal)
            .ToList();
        var declarationNames = CSharpNaming.BuildDeclarationNames(allClassDeclarations);
        var publicDeclarationsByQualifiedName = allClassDeclarations
            .Where(declaration => declaration.Access == "public")
            .GroupBy(declaration => declaration.QualifiedName, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        var allDeclarationsByQualifiedName = records.OfType<DeclarationRecord>()
            .GroupBy(declaration => declaration.QualifiedName, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        var abstractDeclarationIds = records.OfType<MemberRecord>()
            .Where(member => member.Pure)
            .Select(member => member.DeclarationId)
            .ToHashSet(StringComparer.Ordinal);
        var enumUnderlyingByName = records.OfType<DeclarationRecord>()
            .Where(declaration => declaration.Kind == "enum"
                && !string.IsNullOrWhiteSpace(declaration.UnderlyingType))
            .GroupBy(declaration => declaration.QualifiedName, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First().UnderlyingType!, StringComparer.Ordinal);

        var declarations = allClassDeclarations
            .Where(declaration => declaration.Access == "public")
            .Where(declaration => linkCarrier || declaration.Template is null)
            .Where(declaration => IsValidCppQualifiedName(declaration.QualifiedName))
            .Where(declaration => declaration.Template is null
                || (linkCarrier
                    ? !HasInaccessibleTemplateArgument(declaration)
                        && !HasUnsafeIncompleteTemplateArgument(declaration)
                    : AreTemplateArgumentsUsable(declaration, allDeclarationsByQualifiedName)))
            .Where(declaration => declaration.Source is not null)
            .Where(declaration => IsNativeSourceAvailable(nativeRoot, declaration.Source!.File))
            .ToList();

        var membersByDeclaration = records.OfType<MemberRecord>()
            .GroupBy(member => member.DeclarationId, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => group.GroupBy(member => member.Id, StringComparer.Ordinal)
                    .Select(byId => byId.First())
                    .OrderBy(member => member.Id, StringComparer.Ordinal)
                    .ToList(),
                StringComparer.Ordinal);

        var allDeclarationsById = records.OfType<DeclarationRecord>()
            .GroupBy(declaration => declaration.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        var basesByDeclaration = records.OfType<VariantRecord>()
            .GroupBy(variant => variant.DeclarationId, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => group
                    .SelectMany(variant => variant.Layout?.Bases ?? [])
                    .Select(baseInfo => baseInfo.DeclarationId)
                    .Distinct(StringComparer.Ordinal)
                    .ToList(),
                StringComparer.Ordinal);
        var linkSafeMemo = new Dictionary<string, bool>(StringComparer.Ordinal);

        bool IsLinkSafe(string declarationId)
        {
            if (linkSafeMemo.TryGetValue(declarationId, out var cached))
            {
                return cached;
            }

            if (!allDeclarationsById.ContainsKey(declarationId))
            {
                linkSafeMemo[declarationId] = false;
                return false;
            }

            // Break inheritance cycles conservatively.
            linkSafeMemo[declarationId] = false;

            if (membersByDeclaration.TryGetValue(declarationId, out var members)
                && members.Any(member =>
                    (member.Kind is "method" or "operator" or "constructor" or "destructor")
                    && !member.Inline))
            {
                return false;
            }

            if (basesByDeclaration.TryGetValue(declarationId, out var bases))
            {
                foreach (var baseId in bases)
                {
                    if (!IsLinkSafe(baseId))
                    {
                        return false;
                    }
                }
            }

            linkSafeMemo[declarationId] = true;
            return true;
        }

        bool IsDestructorUsable(DeclarationRecord declaration, List<MemberRecord> declarationMembers)
        {
            var destructor = declarationMembers.FirstOrDefault(member => member.Kind == "destructor");
            var hasOutOfLineMethod = declarationMembers.Any(member =>
                (member.Kind is "method" or "operator" or "constructor" or "destructor")
                && !member.Inline);
            return linkCarrier
                ? destructor is null
                    || (destructor.Access == "public"
                        && HasExportedSymbol(declaration, destructor)
                        && (declaration.Template is null || destructor.Defined))
                : destructor is null
                    ? !hasOutOfLineMethod
                    : destructor.Access == "public" && destructor.Inline && !hasOutOfLineMethod;
        }

        bool HasImplicitSpecializationMembers(DeclarationRecord declaration, List<MemberRecord> declarationMembers) =>
            declarationMembers.Any(member =>
                (member.Signature?.Canonical.StartsWith(declaration.QualifiedName + "<", StringComparison.Ordinal) ?? false)
                || member.Id.Contains(declaration.QualifiedName + "<", StringComparison.Ordinal));

        bool CanDeleteImplicitDestructor(
            DeclarationRecord declaration,
            List<MemberRecord> declarationMembers,
            out bool explicitDestructor)
        {
            explicitDestructor = false;
            var destructor = declarationMembers.FirstOrDefault(member => member.Kind == "destructor");
            if (destructor is not null)
            {
                explicitDestructor = destructor.Access == "public"
                    && (linkCarrier ? HasExportedSymbol(declaration, destructor) : destructor.Inline)
                    && (declaration.Template is null || destructor.Defined);
                return explicitDestructor;
            }

            // An implicit destructor is instantiated at the delete site. Member smart pointers to
            // incomplete implementation types make that instantiation ill-formed.
            if (declarationMembers.Any(member =>
                    member.Kind == "field"
                    && member.Type is not null
                    && member.Type.Canonical.StartsWith("std::unique_ptr<", StringComparison.Ordinal)))
            {
                return false;
            }

            var constructors = declarationMembers.Where(member => member.Kind == "constructor").ToList();
            return constructors.Count == 0
                || constructors.Any(member => member.Access == "public");
        }

        var destroyableDeclarationIds = new HashSet<string>(StringComparer.Ordinal);
        foreach (var declaration in declarations)
        {
            if (!membersByDeclaration.TryGetValue(declaration.Id, out var declarationMembers)
                || HasImplicitSpecializationMembers(declaration, declarationMembers)
                || !(linkCarrier || IsLinkSafe(declaration.Id))
                || declaration.Abstract
                || !CanDeleteImplicitDestructor(declaration, declarationMembers, out _))
            {
                continue;
            }

            destroyableDeclarationIds.Add(declaration.Id);
        }

        var functions = new List<AbiFunction>();
        foreach (var declaration in declarations)
        {
            if (!membersByDeclaration.TryGetValue(declaration.Id, out var declarationMembers))
            {
                continue;
            }

            // A primary-template record can carry the members of an implicit specialization.
            // Its native name would then be incomplete (no template arguments), so it is not a
            // safe C++ type for the shim until the IR distinguishes the two.
            if (HasImplicitSpecializationMembers(declaration, declarationMembers))
            {
                continue;
            }

            // A class that stores a string_view/reference field cannot be safely created from
            // managed memory: the native object would outlive the caller's buffer. Its existing
            // handles can still be read through real getters.
            var ownsBorrowedString = declarationMembers.Any(member =>
                member.Kind == "field"
                && member.Type is not null
                && IsStringViewType(member.Type.Canonical));
            var linkSafe = linkCarrier || IsLinkSafe(declaration.Id);
            var destructorUsable = IsDestructorUsable(declaration, declarationMembers);
            var hasPublicDestructor = CanDeleteImplicitDestructor(
                declaration,
                declarationMembers,
                out var explicitDestructor)
                && explicitDestructor;
            var destructorAccessible = linkSafe && destructorUsable;
            var isAbstract = declarationMembers.Any(member => member.Pure);
            var hasAbstractTemplateArgument = declaration.Template is not null
                && HasAbstractTemplateArgument(declaration, allDeclarationsByQualifiedName, abstractDeclarationIds);
            var created = false;

            foreach (var constructor in declarationMembers.Where(member =>
                         member.Kind == "constructor"
                         && member.Access == "public"
                         && (linkCarrier ? HasExportedSymbol(declaration, member) : member.Inline)
                         && (!linkCarrier || declaration.Template is null || member.Defined || !member.Inline)
                         && !member.Pure))
            {
                if (ownsBorrowedString || !destructorAccessible || isAbstract || hasAbstractTemplateArgument)
                {
                    continue;
                }

                if (!TryBuildCreate(declaration, constructor, publicDeclarationsByQualifiedName, declarationNames, enumUnderlyingByName, out var create))
                {
                    continue;
                }

                functions.Add(create);
                created = true;
            }

            if (hasPublicDestructor || created || destroyableDeclarationIds.Contains(declaration.Id))
            {
                functions.Add(BuildDestroy(declaration, declarationNames));
            }

            foreach (var field in declarationMembers.Where(member =>
                         (member.Kind == "field" || (linkCarrier && member.Kind == "static_field"))
                         && member.Access == "public"
                         && !member.VariableTemplate
                         && member.Type is not null))
            {
                if (!TryBuildField(declaration, field, publicDeclarationsByQualifiedName, declarationNames, enumUnderlyingByName, out var getter, out var setter))
                {
                    continue;
                }

                functions.Add(getter);
                if (setter is not null)
                {
                    functions.Add(setter);
                }
            }

            if (!linkSafe)
            {
                continue;
            }

            foreach (var method in declarationMembers.Where(member =>
                         member.Kind is "method" or "operator"
                         && member.Access == "public"
                         && (linkCarrier ? HasExportedSymbol(declaration, member) : member.Inline)
                         && !member.Pure
                         && !member.MemberTemplate
                         && member.RefQualifier != "rvalue"))
            {
                if (!TryBuildMethod(declaration, method, publicDeclarationsByQualifiedName, declarationNames, destroyableDeclarationIds, enumUnderlyingByName, out var function))
                {
                    continue;
                }

                functions.Add(function);
            }
        }

        var ordered = functions
            .GroupBy(function => function.Surface, StringComparer.Ordinal)
            .OrderBy(group => group.Key, StringComparer.Ordinal)
            .SelectMany(group => group
                .OrderBy(function => function.DeclarationId, StringComparer.Ordinal)
                .ThenBy(function => function.MemberId, StringComparer.Ordinal)
                .ThenBy(function => function.Kind)
                .ThenBy(function => function.CppName, StringComparer.Ordinal))
            .ToList();

        var indexBySurface = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var function in ordered)
        {
            indexBySurface.TryGetValue(function.Surface, out var index);
            function.Index = index;
            indexBySurface[function.Surface] = index + 1;
        }

        return new AbiPlan(ordered, declarationNames);
    }

    public static string CppType(AbiKind kind) => kind switch
    {
        AbiKind.Void => "void",
        AbiKind.Bool => "bool",
        AbiKind.I8 => "std::int8_t",
        AbiKind.U8 => "std::uint8_t",
        AbiKind.I16 => "std::int16_t",
        AbiKind.U16 => "std::uint16_t",
        AbiKind.I32 => "std::int32_t",
        AbiKind.U32 => "std::uint32_t",
        AbiKind.I64 => "std::int64_t",
        AbiKind.U64 => "std::uint64_t",
        AbiKind.USize => "std::size_t",
        AbiKind.ISize => "std::ptrdiff_t",
        AbiKind.F32 => "float",
        AbiKind.F64 => "double",
        AbiKind.Opaque => "void*",
        AbiKind.StringView => "HosihikariStringView",
        _ => "void",
    };

    public static string CSharpType(AbiKind kind) => kind switch
    {
        AbiKind.Void => "void",
        AbiKind.Bool => "bool",
        AbiKind.I8 => "sbyte",
        AbiKind.U8 => "byte",
        AbiKind.I16 => "short",
        AbiKind.U16 => "ushort",
        AbiKind.I32 => "int",
        AbiKind.U32 => "uint",
        AbiKind.I64 => "long",
        AbiKind.U64 => "ulong",
        AbiKind.USize => "nuint",
        AbiKind.ISize => "nint",
        AbiKind.F32 => "float",
        AbiKind.F64 => "double",
        AbiKind.Opaque => "nint",
        AbiKind.StringView => "string",
        _ => "nint",
    };

    public static string CSharpTableType(AbiKind kind) => kind switch
    {
        AbiKind.Opaque => "void*",
        AbiKind.StringView => "NativeMethods.StringView",
        _ => CSharpType(kind),
    };

    public static string CSharpParameterType(AbiParameter parameter) => parameter.Kind switch
    {
        AbiKind.StringView => "string",
        AbiKind.Opaque => parameter.WrapperType ?? "nint",
        _ => CSharpType(parameter.Kind),
    };

    public static string CSharpPropertyType(AbiKind kind) => kind switch
    {
        AbiKind.StringView => "string",
        AbiKind.Opaque => "nint",
        _ => CSharpType(kind),
    };

    public static string CppParameterType(AbiParameter parameter) => parameter.Kind switch
    {
        AbiKind.Opaque => "void*",
        AbiKind.StringView => "const char*, std::size_t",
        _ => CppType(parameter.Kind),
    };

    public static bool IsStringViewType(string? canonical)
    {
        if (canonical is null)
        {
            return false;
        }

        if (StripTopLevelQualifiers(canonical).EndsWith("*", StringComparison.Ordinal))
        {
            return false;
        }

        var normalized = NormalizeTypeName(canonical);
        return string.Equals(normalized, "std::basic_string_view<char>", StringComparison.Ordinal)
            || string.Equals(normalized, "std::string_view", StringComparison.Ordinal);
    }

    private static bool IsStringType(string canonical)
    {
        if (StripTopLevelQualifiers(canonical).EndsWith("*", StringComparison.Ordinal))
        {
            return false;
        }

        var normalized = NormalizeTypeName(canonical);
        return string.Equals(normalized, "std::basic_string<char>", StringComparison.Ordinal)
            || string.Equals(normalized, "std::string", StringComparison.Ordinal);
    }

    private static string NormalizeTypeName(string canonical)
    {
        var text = StripTopLevelQualifiers(canonical);
        while (text.EndsWith("&", StringComparison.Ordinal) || text.EndsWith("*", StringComparison.Ordinal))
        {
            text = StripTopLevelQualifiers(text[..^1].TrimEnd());
        }

        return text;
    }

    private static string StripTopLevelQualifiers(string canonical)
    {
        var text = canonical.Trim();
        var changed = true;
        while (changed)
        {
            changed = false;
            if (text.StartsWith("const ", StringComparison.Ordinal))
            {
                text = text["const ".Length..].TrimStart();
                changed = true;
            }

            if (text.StartsWith("volatile ", StringComparison.Ordinal))
            {
                text = text["volatile ".Length..].TrimStart();
                changed = true;
            }

            if (text.EndsWith(" const", StringComparison.Ordinal))
            {
                text = text[..^" const".Length].TrimEnd();
                changed = true;
            }

            if (text.EndsWith(" volatile", StringComparison.Ordinal))
            {
                text = text[..^" volatile".Length].TrimEnd();
                changed = true;
            }
        }

        return text;
    }

    private static bool HasExportedSymbol(DeclarationRecord declaration, MemberRecord member) =>
        member.Inline
        || HasExportAttribute(declaration.Attributes)
        || HasExportAttribute(member.Attributes)
        || HasExportMacro(declaration.Macros)
        || HasExportMacro(member.Macros);

    private static bool HasExportAttribute(IEnumerable<AttributeInfo> attributes) =>
        attributes.Any(attribute =>
            attribute.Name.Equals("dllexport", StringComparison.OrdinalIgnoreCase)
            || attribute.Name.Equals("dllimport", StringComparison.OrdinalIgnoreCase));

    private static bool HasExportMacro(IEnumerable<MacroUseInfo> macros) =>
        macros.Any(macro => macro.Name is "LLAPI" or "LLNDAPI" or "LLCAPI" or "MCAPI" or "MCNAPI" or "ENDSTONE_EVENT");

    internal static string NormalizeTemplateTypeName(string value) => NormalizeTypeName(value);

    internal static bool IsFundamentalTemplateArgument(string type) =>
        TryMapFundamental(type, out _) || IsScalarAliasType(type);

    internal static bool IsStandardOrThirdPartyType(string type) =>
        type.StartsWith("std::", StringComparison.Ordinal)
        || type.StartsWith("gsl::", StringComparison.Ordinal)
        || type.StartsWith("fmt::", StringComparison.Ordinal)
        || type.StartsWith("entt::", StringComparison.Ordinal)
        || type.StartsWith("nonstd::", StringComparison.Ordinal)
        || type.StartsWith("expected_lite::", StringComparison.Ordinal)
        || type.StartsWith("boost::", StringComparison.Ordinal)
        || type.StartsWith("cereal::", StringComparison.Ordinal)
        || type.StartsWith("phmap::", StringComparison.Ordinal)
        || type.StartsWith("magic_enum::", StringComparison.Ordinal)
        || type.StartsWith("type_safe::", StringComparison.Ordinal);

    internal static bool IsLiteralTemplateArgument(string text)
    {
        var value = text.Trim();
        if (value is "true" or "false" or "nullptr" or "NULL")
        {
            return true;
        }

        value = value.TrimStart('-', '+');
        if (value.Length == 0)
        {
            return false;
        }

        if (value.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            return value.Length > 2 && value.Skip(2).All(Uri.IsHexDigit);
        }

        return value.All(current =>
            char.IsDigit(current) || current is '.' or 'U' or 'u' or 'L' or 'l' or 'F' or 'f');
    }

    internal static string? ExtractTemplateArguments(string type)
    {
        var open = type.IndexOf('<');
        if (open < 0)
        {
            return null;
        }

        var depth = 0;
        for (var index = open; index < type.Length; index++)
        {
            if (type[index] == '<')
            {
                depth++;
            }
            else if (type[index] == '>')
            {
                depth--;
                if (depth == 0)
                {
                    return index == type.Length - 1 ? type[(open + 1)..index] : null;
                }
            }
        }

        return null;
    }

    internal static IEnumerable<string> SplitTemplateArguments(string inner)
    {
        if (inner.Length == 0)
        {
            yield break;
        }

        var depth = 0;
        var start = 0;
        for (var index = 0; index < inner.Length; index++)
        {
            var current = inner[index];
            if (current == '<')
            {
                depth++;
            }
            else if (current == '>')
            {
                depth--;
            }
            else if (current == ',' && depth == 0)
            {
                yield return inner[start..index].Trim();
                start = index + 1;
            }
        }

        yield return inner[start..].Trim();
    }

    private static bool IsScalarAliasType(string type) => type is
        "uint8" or "uint16" or "uint32" or "uint64"
        or "int8" or "int16" or "int32" or "int64"
        or "uchar" or "ushort" or "uint" or "ulong" or "ulonglong"
        or "schar" or "ssize_t" or "intptr_t" or "uintptr_t"
        or "wchar_t" or "char8_t" or "char16_t" or "char32_t";

    private static bool HasAbstractTemplateArgument(
        DeclarationRecord declaration,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        HashSet<string> abstractDeclarationIds)
    {
        if (declaration.Template is null)
        {
            return false;
        }

        var visited = new HashSet<string>(StringComparer.Ordinal);
        foreach (var argument in declaration.Template.Arguments)
        {
            if (string.Equals(argument.Kind, "pack", StringComparison.Ordinal))
            {
                foreach (var part in PackArgumentParts(argument.Value))
                {
                    if (IsAbstractTemplateType(part, declarationsByQualifiedName, abstractDeclarationIds, visited))
                    {
                        return true;
                    }
                }
            }
            else if (string.Equals(argument.Kind, "type", StringComparison.Ordinal)
                && IsAbstractTemplateType(argument.Value, declarationsByQualifiedName, abstractDeclarationIds, visited))
            {
                return true;
            }
        }

        return false;
    }

    private static bool IsAbstractTemplateType(
        string value,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        HashSet<string> abstractDeclarationIds,
        HashSet<string> visited)
    {
        var type = NormalizeTypeName(value);
        if (string.IsNullOrWhiteSpace(type) || !visited.Add(type))
        {
            return false;
        }

        if (declarationsByQualifiedName.TryGetValue(type, out var declaration))
        {
            if (abstractDeclarationIds.Contains(declaration.Id))
            {
                return true;
            }

            if (declaration.Template is not null)
            {
                foreach (var argument in declaration.Template.Arguments)
                {
                    if (string.Equals(argument.Kind, "type", StringComparison.Ordinal)
                        && IsAbstractTemplateType(
                            argument.Value,
                            declarationsByQualifiedName,
                            abstractDeclarationIds,
                            visited))
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        if (!IsStandardOrThirdPartyType(type))
        {
            return false;
        }

        var inner = ExtractTemplateArguments(type);
        if (inner is null)
        {
            return false;
        }

        return SplitTemplateArguments(inner).Any(part =>
            IsAbstractTemplateType(part, declarationsByQualifiedName, abstractDeclarationIds, visited));
    }

    private static bool HasInaccessibleTemplateArgument(DeclarationRecord declaration) =>
        declaration.Template?.Arguments.Any(argument =>
            argument.Accessible.HasValue && !argument.Accessible.Value) ?? false;

    private static bool HasUnsafeIncompleteTemplateArgument(DeclarationRecord declaration) =>
        declaration.Template?.Arguments.Any(argument =>
            argument.Complete.HasValue
            && !argument.Complete.Value
            && !IsStandardOrThirdPartyType(NormalizeTypeName(argument.Value))) ?? false;

    private static bool AreTemplateArgumentsUsable(
        DeclarationRecord declaration,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName)
    {
        if (declaration.Template is null)
        {
            return true;
        }

        var visited = new HashSet<string>(StringComparer.Ordinal);
        foreach (var argument in declaration.Template.Arguments)
        {
            if (string.Equals(argument.Kind, "pack", StringComparison.Ordinal))
            {
                foreach (var part in PackArgumentParts(argument.Value))
                {
                    if (!IsUsableTemplateType(part, declarationsByQualifiedName, visited))
                    {
                        return false;
                    }
                }
            }
            else if (string.Equals(argument.Kind, "type", StringComparison.Ordinal)
                && !IsUsableTemplateType(argument.Value, declarationsByQualifiedName, visited))
            {
                return false;
            }
        }

        return true;
    }

    internal static IEnumerable<string> PackArgumentParts(string value)
    {
        var inner = ExtractTemplateArguments(value);
        return inner is null ? [value] : SplitTemplateArguments(inner);
    }

    private static bool IsUsableTemplateType(
        string value,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        HashSet<string> visited)
    {
        var type = NormalizeTypeName(value);
        if (string.IsNullOrWhiteSpace(type))
        {
            return false;
        }

        if (IsFundamentalTemplateArgument(type) || IsLiteralTemplateArgument(type))
        {
            return true;
        }

        if (!visited.Add(type))
        {
            return true;
        }

        if (declarationsByQualifiedName.TryGetValue(type, out var declaration))
        {
            if (declaration.Template is not null)
            {
                foreach (var argument in declaration.Template.Arguments)
                {
                    if (string.Equals(argument.Kind, "type", StringComparison.Ordinal)
                        && !IsUsableTemplateType(argument.Value, declarationsByQualifiedName, visited))
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        if (IsStandardOrThirdPartyType(type))
        {
            var inner = ExtractTemplateArguments(type);
            if (inner is null)
            {
                return true;
            }

            foreach (var part in SplitTemplateArguments(inner))
            {
                if (!IsUsableTemplateType(part, declarationsByQualifiedName, visited))
                {
                    return false;
                }
            }

            return true;
        }

        return false;
    }

    private static bool IsValidCppQualifiedName(string qualifiedName)
    {
        if (string.IsNullOrWhiteSpace(qualifiedName)
            || qualifiedName.Contains("...", StringComparison.Ordinal)
            || qualifiedName.Contains("decltype", StringComparison.Ordinal)
            || qualifiedName.Contains("auto", StringComparison.Ordinal)
            || qualifiedName.Contains("__attribute", StringComparison.Ordinal))
        {
            return false;
        }

        var parts = TypeNames.SplitQualifiedName(qualifiedName);
        if (parts.Count == 0)
        {
            return false;
        }

        foreach (var part in parts)
        {
            if (!IsValidCppNamePart(part))
            {
                return false;
            }
        }

        return true;
    }

    private static bool IsValidCppNamePart(string part)
    {
        var index = 0;
        if (!ReadCppIdentifier(part, ref index))
        {
            return false;
        }

        if (index == part.Length)
        {
            return true;
        }

        if (part[index] != '<')
        {
            return false;
        }

        var depth = 0;
        for (; index < part.Length; index++)
        {
            var current = part[index];
            if (current == '<')
            {
                depth++;
                continue;
            }

            if (current == '>')
            {
                if (depth == 0)
                {
                    return false;
                }

                depth--;
                continue;
            }

            if (depth == 0)
            {
                return false;
            }

            if (current is '(' or ')' or '{' or '}' or '[' or ']' or '"' or '\''
                or ';' or '=' or '@' or '\\' or '#' or '?' or '!' or '%' or '^'
                or '|' or '~' or '`')
            {
                return false;
            }

            if (!(char.IsLetterOrDigit(current)
                  || current is '_' or ':' or ',' or ' ' or '*' or '&' or '-' or '+' or '.'))
            {
                return false;
            }
        }

        return depth == 0;
    }

    private static bool ReadCppIdentifier(string text, ref int index)
    {
        if (index >= text.Length || (text[index] != '_' && !char.IsLetter(text[index])))
        {
            return false;
        }

        index++;
        while (index < text.Length && (text[index] == '_' || char.IsLetterOrDigit(text[index])))
        {
            index++;
        }

        return true;
    }

    private static bool IsNativeSourceAvailable(string nativeRoot, string sourceFile)
    {
        if (string.IsNullOrWhiteSpace(sourceFile))
        {
            return false;
        }

        var normalized = sourceFile.Replace('\\', '/').TrimStart('/');
        var fullPath = Path.GetFullPath(Path.Combine(nativeRoot, normalized));
        return File.Exists(fullPath);
    }

    private static bool TryBuildCreate(
        DeclarationRecord declaration,
        MemberRecord constructor,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        IReadOnlyDictionary<string, DeclarationName> declarationNames,
        IReadOnlyDictionary<string, string> enumUnderlyingByName,
        out AbiFunction function)
    {
        function = null!;
        if (constructor.Signature is null || constructor.Signature.Variadic || constructor.Consteval)
        {
            return false;
        }

        var parameters = new List<AbiParameter>();
        var stringParameters = 0;
        foreach (var parameter in constructor.Signature.Parameters)
        {
            if (!TryMapParameter(
                    parameter.Name,
                    parameter.Type,
                    declarationsByQualifiedName,
                    declarationNames,
                    enumUnderlyingByName,
                    out var abiParameter))
            {
                return false;
            }

            if (abiParameter.Kind == AbiKind.StringView)
            {
                stringParameters++;
                if (stringParameters > 1 || IsNonConstLValueReference(parameter.Type))
                {
                    // The shim would have to bind a temporary string to a mutable reference,
                    // and v1 pins at most one string buffer per call.
                    return false;
                }
            }

            if (abiParameter.Kind == AbiKind.Opaque
                && abiParameter.IsReference
                && !abiParameter.IsConst)
            {
                // Copy-like constructors that take a mutable reference can instantiate
                // unrelated templates (for example AnyFunction(AnyFunction&)).
                return false;
            }

            parameters.Add(abiParameter);
        }

        function = new AbiFunction
        {
            CppName = "hs_create_" + CSharpNaming.ShortHash(constructor.Id),
            Kind = AbiFunctionKind.Create,
            Surface = declaration.Surface,
            DeclarationId = declaration.Id,
            MemberId = constructor.Id,
            ClassCppName = declaration.QualifiedName,
            ClassName = declarationNames[declaration.Id],
            NativeMemberName = constructor.Name,
            CSharpMemberName = "Create",
            ReturnKind = AbiKind.Opaque,
            NativeReturnType = "void*",
            Parameters = parameters,
        };
        return true;
    }

    private static AbiFunction BuildDestroy(
        DeclarationRecord declaration,
        IReadOnlyDictionary<string, DeclarationName> declarationNames) =>
        new()
        {
            CppName = "hs_destroy_" + CSharpNaming.ShortHash(declaration.Id),
            Kind = AbiFunctionKind.Destroy,
            Surface = declaration.Surface,
            DeclarationId = declaration.Id,
            ClassCppName = declaration.QualifiedName,
            ClassName = declarationNames[declaration.Id],
            ReturnKind = AbiKind.Void,
            NativeReturnType = "void",
        };

    private static bool TryBuildMethod(
        DeclarationRecord declaration,
        MemberRecord method,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        IReadOnlyDictionary<string, DeclarationName> declarationNames,
        IReadOnlySet<string> destroyableDeclarationIds,
        IReadOnlyDictionary<string, string> enumUnderlyingByName,
        out AbiFunction function)
    {
        function = null!;
        if (method.Signature is null
            || method.Signature.Variadic
            || method.Consteval
            || string.Equals(method.Name, "operator=", StringComparison.Ordinal))
        {
            return false;
        }

        if (!TryMapReturnType(
                method.Signature.ReturnType,
                declarationsByQualifiedName,
                declarationNames,
                destroyableDeclarationIds,
                enumUnderlyingByName,
                out var returnKind,
                out var nativeReturnType,
                out var returnWrapperType,
                out var returnOwned))
        {
            return false;
        }

        var parameters = new List<AbiParameter>();
        var stringParameters = 0;
        foreach (var parameter in method.Signature.Parameters)
        {
            if (!TryMapParameter(
                    parameter.Name,
                    parameter.Type,
                    declarationsByQualifiedName,
                    declarationNames,
                    enumUnderlyingByName,
                    out var abiParameter))
            {
                return false;
            }

            if (abiParameter.Kind == AbiKind.StringView)
            {
                stringParameters++;
                if (stringParameters > 1 || IsNonConstLValueReference(parameter.Type))
                {
                    return false;
                }
            }

            parameters.Add(abiParameter);
        }

        function = new AbiFunction
        {
            CppName = "hs_method_" + CSharpNaming.ShortHash(method.Id),
            Kind = AbiFunctionKind.Method,
            Surface = declaration.Surface,
            DeclarationId = declaration.Id,
            MemberId = method.Id,
            ClassCppName = declaration.QualifiedName,
            ClassName = declarationNames[declaration.Id],
            NativeMemberName = method.Name,
            CSharpMemberName = TypeNames.Sanitize(method.Name),
            ReturnKind = returnKind,
            NativeReturnType = nativeReturnType,
            ReturnWrapperType = returnWrapperType,
            ReturnOwned = returnOwned,
            Parameters = parameters,
            Static = method.Static,
        };
        return true;
    }

    private static bool TryMapReturnType(
        string canonical,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        IReadOnlyDictionary<string, DeclarationName> declarationNames,
        IReadOnlySet<string> destroyableDeclarationIds,
        IReadOnlyDictionary<string, string> enumUnderlyingByName,
        out AbiKind kind,
        out string nativeType,
        out string? wrapperType,
        out bool owned)
    {
        kind = AbiKind.Void;
        nativeType = canonical.Trim();
        wrapperType = null;
        owned = false;
        if (nativeType.Length == 0)
        {
            return false;
        }

        if (IsStringViewType(nativeType) || IsStringType(nativeType))
        {
            kind = AbiKind.StringView;
            return true;
        }

        if (TryMapEnumUnderlying(nativeType, enumUnderlyingByName, out kind))
        {
            return true;
        }

        if (TryMapFundamental(nativeType, out kind))
        {
            return true;
        }

        if (IsPointerOrReferenceType(nativeType))
        {
            kind = AbiKind.Opaque;
            return true;
        }

        if (!IsByValueRecordType(nativeType))
        {
            return false;
        }

        // A by-value record result is copied into a shim-owned object. The copy can only be
        // released through the class emitted destroy function, so require an accessible one.
        var byValueName = StripTopLevelQualifiers(nativeType);
        if (!declarationsByQualifiedName.TryGetValue(byValueName, out var resultDeclaration)
            || resultDeclaration.Abstract
            || !destroyableDeclarationIds.Contains(resultDeclaration.Id)
            || !declarationNames.TryGetValue(resultDeclaration.Id, out var resultName))
        {
            return false;
        }

        kind = AbiKind.Opaque;
        nativeType = byValueName;
        wrapperType = "global::" + resultName.NamespaceName + "." + resultName.ClassName;
        owned = true;
        return true;
    }

    private static bool TryBuildField(
        DeclarationRecord declaration,
        MemberRecord field,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        IReadOnlyDictionary<string, DeclarationName> declarationNames,
        IReadOnlyDictionary<string, string> enumUnderlyingByName,
        out AbiFunction getter,
        out AbiFunction? setter)
    {
        getter = null!;
        setter = null;
        var type = field.Type!;
        var text = type.Canonical.Trim();
        var kind = AbiKind.Void;
        if (IsStringViewType(text) || IsStringType(text))
        {
            kind = AbiKind.StringView;
        }
        else if (!TryMapEnumUnderlying(text, enumUnderlyingByName, out kind)
            && !TryMapFundamental(text, out kind))
        {
            if (!TryMapOpaque(text, declarationsByQualifiedName, declarationNames, out var pointee, out var wrapperType))
            {
                if (!IsByValueRecordType(text) && !IsArrayType(text))
                {
                    return false;
                }

                pointee = StripTopLevelQualifiers(text);
                wrapperType = LookupWrapperType(text, declarationsByQualifiedName, declarationNames);
            }

            kind = AbiKind.Opaque;
            _ = pointee;
            _ = wrapperType;
        }

        var name = string.IsNullOrWhiteSpace(field.Name) ? "value" : field.Name;
        getter = new AbiFunction
        {
            CppName = "hs_get_" + CSharpNaming.ShortHash(field.Id),
            Kind = AbiFunctionKind.Get,
            Surface = declaration.Surface,
            DeclarationId = declaration.Id,
            MemberId = field.Id,
            ClassCppName = declaration.QualifiedName,
            ClassName = declarationNames[declaration.Id],
            NativeMemberName = field.Name,
            CSharpMemberName = TypeNames.Sanitize(name),
            ReturnKind = kind,
            NativeReturnType = text,
            Static = field.Static,
        };

        if (kind is AbiKind.StringView || type.Const || IsConstType(text) || IsArrayType(text))
        {
            return true;
        }

        if (!TryMapParameter(name, text, declarationsByQualifiedName, declarationNames, enumUnderlyingByName, out var valueParameter))
        {
            return true;
        }

        setter = new AbiFunction
        {
            CppName = "hs_set_" + CSharpNaming.ShortHash(field.Id),
            Kind = AbiFunctionKind.Set,
            Surface = declaration.Surface,
            DeclarationId = declaration.Id,
            MemberId = field.Id,
            ClassCppName = declaration.QualifiedName,
            ClassName = declarationNames[declaration.Id],
            NativeMemberName = field.Name,
            CSharpMemberName = TypeNames.Sanitize(name),
            ReturnKind = AbiKind.Void,
            NativeReturnType = "void",
            Static = field.Static,
            Parameters = [valueParameter],
        };
        return true;
    }

    private static bool TryMapParameter(
        string name,
        string canonical,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        IReadOnlyDictionary<string, DeclarationName> declarationNames,
        IReadOnlyDictionary<string, string> enumUnderlyingByName,
        out AbiParameter parameter)
    {
        parameter = null!;
        if (string.IsNullOrWhiteSpace(canonical))
        {
            return false;
        }

        var text = canonical.Trim();
        if (IsStringViewType(text) || IsStringType(text))
        {
            parameter = new AbiParameter
            {
                Name = name,
                Kind = AbiKind.StringView,
                NativeType = text,
            };
            return true;
        }

        if (TryMapEnumUnderlying(text, enumUnderlyingByName, out var kind)
            || TryMapFundamental(text, out kind))
        {
            parameter = new AbiParameter
            {
                Name = name,
                Kind = kind,
                NativeType = text,
            };
            return true;
        }

        if (TryMapOpaque(text, declarationsByQualifiedName, declarationNames, out var pointee, out var wrapperType))
        {
            parameter = new AbiParameter
            {
                Name = name,
                Kind = AbiKind.Opaque,
                NativeType = text,
                NativePointee = pointee,
                IsReference = IsReferenceType(text),
                IsRValueReference = IsRValueReference(text),
                IsConst = IsConstType(text),
                WrapperType = wrapperType,
            };
            return true;
        }

        if (IsByValueRecordType(text))
        {
            var byValueName = StripTopLevelQualifiers(text);
            if (declarationsByQualifiedName.TryGetValue(byValueName, out var byValueDeclaration)
                && !byValueDeclaration.Abstract
                && byValueDeclaration.CopyConstructible)
            {
                parameter = new AbiParameter
                {
                    Name = name,
                    Kind = AbiKind.Opaque,
                    NativeType = text,
                    NativePointee = byValueName,
                    IsReference = true,
                    IsConst = true,
                    WrapperType = LookupWrapperType(text, declarationsByQualifiedName, declarationNames),
                };
                return true;
            }

            return false;
        }

        return false;
    }

    private static string? LookupWrapperType(
        string text,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        IReadOnlyDictionary<string, DeclarationName> declarationNames)
    {
        var name = StripTopLevelQualifiers(text);
        while (name.EndsWith("&", StringComparison.Ordinal) || name.EndsWith("*", StringComparison.Ordinal))
        {
            name = StripTopLevelQualifiers(name[..^1].TrimEnd());
        }

        return declarationsByQualifiedName.TryGetValue(name, out var declaration)
            && declarationNames.TryGetValue(declaration.Id, out var declarationName)
                ? "global::" + declarationName.NamespaceName + "." + declarationName.ClassName
                : null;
    }

    private static bool TryMapOpaque(
        string text,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        IReadOnlyDictionary<string, DeclarationName> declarationNames,
        out string? pointee,
        out string? wrapperType)
    {
        pointee = null;
        wrapperType = null;
        if (!IsPointerOrReferenceType(text))
        {
            return false;
        }

        pointee = ExtractPointee(text);
        if (pointee.Length == 0)
        {
            return false;
        }

        if (declarationsByQualifiedName.TryGetValue(pointee, out var declaration)
            && declarationNames.TryGetValue(declaration.Id, out var name))
        {
            wrapperType = "global::" + name.NamespaceName + "." + name.ClassName;
        }

        return true;
    }

    private static bool TryMapEnumUnderlying(
        string canonical,
        IReadOnlyDictionary<string, string> enumUnderlyingByName,
        out AbiKind kind)
    {
        kind = AbiKind.Void;
        var text = canonical.Trim();
        if (text.StartsWith("const ", StringComparison.Ordinal))
        {
            text = text["const ".Length..].TrimStart();
        }

        if (text.EndsWith(" const", StringComparison.Ordinal))
        {
            text = text[..^" const".Length].TrimEnd();
        }

        return !IsPointerOrReferenceType(text)
            && enumUnderlyingByName.TryGetValue(text, out var underlying)
            && TryMapFundamental(underlying, out kind);
    }

    private static bool TryMapFundamental(string canonical, out AbiKind kind)
    {
        var text = canonical.Trim();
        if (text.StartsWith("const ", StringComparison.Ordinal))
        {
            text = text["const ".Length..].TrimStart();
        }

        if (text.EndsWith(" const", StringComparison.Ordinal))
        {
            text = text[..^" const".Length].TrimEnd();
        }

        kind = text switch
        {
            "void" => AbiKind.Void,
            "bool" => AbiKind.Bool,
            "char" or "signed char" or "int8_t" => AbiKind.I8,
            "unsigned char" or "uint8_t" or "char8_t" => AbiKind.U8,
            "short" or "short int" or "signed short" or "signed short int" or "int16_t" => AbiKind.I16,
            "unsigned short" or "unsigned short int" or "uint16_t" => AbiKind.U16,
            "int" or "signed" or "signed int" or "int32_t" => AbiKind.I32,
            "unsigned" or "unsigned int" or "uint32_t" => AbiKind.U32,
            "long long" or "long long int" or "signed long long" or "int64_t" or "__int64" => AbiKind.I64,
            "unsigned long long" or "unsigned long long int" or "uint64_t" or "unsigned __int64" => AbiKind.U64,
            "size_t" or "std::size_t" => AbiKind.USize,
            "ptrdiff_t" or "std::ptrdiff_t" => AbiKind.ISize,
            "float" => AbiKind.F32,
            "double" => AbiKind.F64,
            _ => AbiKind.Void,
        };
        return kind != AbiKind.Void || string.Equals(text, "void", StringComparison.Ordinal);
    }

    private static bool IsPointerOrReferenceType(string canonical)
    {
        var text = StripTopLevelQualifiers(canonical);
        return text.EndsWith("&", StringComparison.Ordinal)
            || text.EndsWith("*", StringComparison.Ordinal);
    }

    internal static bool IsReferenceType(string canonical) =>
        StripTopLevelQualifiers(canonical).EndsWith("&", StringComparison.Ordinal);

    internal static bool IsPointerType(string canonical) =>
        StripTopLevelQualifiers(canonical).EndsWith("*", StringComparison.Ordinal);

    internal static bool IsRValueReference(string canonical) =>
        StripTopLevelQualifiers(canonical).EndsWith("&&", StringComparison.Ordinal);

    internal static bool IsArrayType(string canonical) =>
        StripTopLevelQualifiers(canonical).Contains('[', StringComparison.Ordinal);

    private static bool IsByValueRecordType(string canonical)
    {
        var text = StripTopLevelQualifiers(canonical);
        if (text.Length == 0
            || string.Equals(text, "void", StringComparison.Ordinal)
            || text.Contains('(', StringComparison.Ordinal)
            || text.Contains(')', StringComparison.Ordinal)
            || text.Contains('[', StringComparison.Ordinal)
            || text.Contains(']', StringComparison.Ordinal)
            || text.Contains("...", StringComparison.Ordinal)
            || text.StartsWith("decltype", StringComparison.Ordinal)
            || text.EndsWith("*", StringComparison.Ordinal)
            || text.EndsWith("&", StringComparison.Ordinal))
        {
            return false;
        }

        return true;
    }

    private static bool IsNonConstLValueReference(string canonical)
    {
        var text = canonical.Trim();
        return text.EndsWith("&", StringComparison.Ordinal)
            && !text.EndsWith("&&", StringComparison.Ordinal)
            && !IsConstType(text);
    }

    private static bool IsConstType(string canonical)
    {
        var text = canonical.Trim();
        return text.StartsWith("const ", StringComparison.Ordinal)
            || text.Contains(" const ", StringComparison.Ordinal)
            || text.EndsWith(" const", StringComparison.Ordinal);
    }

    private static string ExtractPointee(string canonical)
    {
        var text = StripTopLevelQualifiers(canonical);
        while (text.EndsWith("&", StringComparison.Ordinal) || text.EndsWith("*", StringComparison.Ordinal))
        {
            text = StripTopLevelQualifiers(text[..^1].TrimEnd());
        }

        return text;
    }
}
