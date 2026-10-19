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

    public static AbiPlan Build(IReadOnlyList<IrRecord> records, string? nativeRoot)
    {
        if (string.IsNullOrWhiteSpace(nativeRoot) || !Directory.Exists(nativeRoot))
        {
            return Empty;
        }

        var declarations = records.OfType<DeclarationRecord>()
            .Where(declaration => declaration.Kind is "class" or "struct" or "union")
            .Where(declaration => declaration.Template is null)
            .Where(declaration => declaration.Access == "public")
            .Where(declaration => IsValidCppQualifiedName(declaration.QualifiedName))
            .Where(declaration => declaration.Source is not null)
            .Where(declaration => IsNativeSourceAvailable(nativeRoot, declaration.Source!.File))
            .OrderBy(declaration => declaration.Id, StringComparer.Ordinal)
            .ToList();

        var declarationNames = CSharpNaming.BuildDeclarationNames(declarations);
        var declarationsByQualifiedName = declarations
            .GroupBy(declaration => declaration.QualifiedName, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);

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
            var isTemplateSpecialization = declarationMembers.Any(member =>
                (member.Signature?.Canonical.StartsWith(declaration.QualifiedName + "<", StringComparison.Ordinal) ?? false)
                || member.Id.Contains(declaration.QualifiedName + "<", StringComparison.Ordinal));
            if (isTemplateSpecialization)
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
            var destructor = declarationMembers.FirstOrDefault(member => member.Kind == "destructor");
            var hasOutOfLineMethod = declarationMembers.Any(member =>
                (member.Kind is "method" or "operator" or "constructor" or "destructor")
                && !member.Inline);
            var linkSafe = IsLinkSafe(declaration.Id);
            var destructorAccessible = linkSafe
                && (destructor is null
                    ? !hasOutOfLineMethod
                    : destructor.Access == "public" && destructor.Inline && !hasOutOfLineMethod);
            var hasPublicDestructor = linkSafe
                && destructor is not null
                && destructor.Access == "public"
                && destructor.Inline
                && !hasOutOfLineMethod;
            var isAbstract = declarationMembers.Any(member => member.Pure);
            var created = false;

            foreach (var constructor in declarationMembers.Where(member =>
                         member.Kind == "constructor"
                         && member.Access == "public"
                         && member.Inline
                         && !member.Pure))
            {
                if (ownsBorrowedString || !destructorAccessible || isAbstract)
                {
                    continue;
                }

                if (!TryBuildCreate(declaration, constructor, declarationsByQualifiedName, declarationNames, out var create))
                {
                    continue;
                }

                functions.Add(create);
                created = true;
            }

            if (hasPublicDestructor || created)
            {
                functions.Add(BuildDestroy(declaration, declarationNames));
            }

            foreach (var field in declarationMembers.Where(member =>
                         member.Kind == "field"
                         && member.Access == "public"
                         && member.Type is not null))
            {
                if (!TryBuildField(declaration, field, declarationsByQualifiedName, declarationNames, out var getter, out var setter))
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
                         && member.Inline
                         && !member.Pure))
            {
                if (!TryBuildMethod(declaration, method, declarationsByQualifiedName, declarationNames, out var function))
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

        var normalized = NormalizeTypeName(canonical);
        return string.Equals(normalized, "std::basic_string_view<char>", StringComparison.Ordinal)
            || string.Equals(normalized, "std::string_view", StringComparison.Ordinal);
    }

    private static bool IsStringType(string canonical)
    {
        var normalized = NormalizeTypeName(canonical);
        return string.Equals(normalized, "std::basic_string<char>", StringComparison.Ordinal)
            || string.Equals(normalized, "std::string", StringComparison.Ordinal);
    }

    private static string NormalizeTypeName(string canonical)
    {
        var text = canonical.Trim();
        while (text.EndsWith("&", StringComparison.Ordinal) || text.EndsWith("*", StringComparison.Ordinal))
        {
            text = text[..^1].TrimEnd();
        }

        if (text.StartsWith("const ", StringComparison.Ordinal))
        {
            text = text["const ".Length..].TrimStart();
        }

        if (text.EndsWith(" const", StringComparison.Ordinal))
        {
            text = text[..^" const".Length].TrimEnd();
        }

        return text;
    }

    private static bool IsValidCppQualifiedName(string qualifiedName)
    {
        if (string.IsNullOrWhiteSpace(qualifiedName))
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
            if (part.Length == 0 || (part[0] != '_' && !char.IsLetter(part[0])))
            {
                return false;
            }

            for (var index = 1; index < part.Length; index++)
            {
                if (part[index] != '_' && !char.IsLetterOrDigit(part[index]))
                {
                    return false;
                }
            }
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
        out AbiFunction function)
    {
        function = null!;
        if (constructor.Signature is null || constructor.Signature.Variadic)
        {
            return false;
        }

        var parameters = new List<AbiParameter>();
        if (constructor.Signature.Parameters.Count > 1)
        {
            return false;
        }

        if (constructor.Signature.Parameters.Count == 1)
        {
            var parameter = constructor.Signature.Parameters[0];
            if (!TryMapParameter(
                    parameter.Name,
                    parameter.Type,
                    declarationsByQualifiedName,
                    declarationNames,
                    out var abiParameter))
            {
                return false;
            }

            if (abiParameter.Kind == AbiKind.StringView
                && IsNonConstLValueReference(parameter.Type))
            {
                // The shim would have to bind a temporary string to a mutable reference;
                // the native constructor could retain or modify it.
                return false;
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
        out AbiFunction function)
    {
        function = null!;
        if (method.Signature is null || method.Signature.Variadic)
        {
            return false;
        }

        if (!TryMapReturnType(method.Signature.ReturnType, out var returnKind, out var nativeReturnType))
        {
            return false;
        }

        var parameters = new List<AbiParameter>();
        if (method.Signature.Parameters.Count > 1)
        {
            return false;
        }

        if (method.Signature.Parameters.Count == 1)
        {
            var parameter = method.Signature.Parameters[0];
            if (!TryMapParameter(
                    parameter.Name,
                    parameter.Type,
                    declarationsByQualifiedName,
                    declarationNames,
                    out var abiParameter))
            {
                return false;
            }

            // A method may retain a borrowed string; keep string parameters out of the
            // first method ABI until an owned/borrowed contract exists.
            if (abiParameter.Kind == AbiKind.StringView)
            {
                return false;
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
            Parameters = parameters,
            Static = method.Static,
        };
        return true;
    }

    private static bool TryMapReturnType(string canonical, out AbiKind kind, out string nativeType)
    {
        kind = AbiKind.Void;
        nativeType = canonical.Trim();
        if (nativeType.Length == 0)
        {
            return false;
        }

        // A returned view into a temporary or into an unknown owner cannot cross the ABI.
        if (IsStringViewType(nativeType) || IsStringType(nativeType))
        {
            return false;
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

        return false;
    }

    private static bool TryBuildField(
        DeclarationRecord declaration,
        MemberRecord field,
        IReadOnlyDictionary<string, DeclarationRecord> declarationsByQualifiedName,
        IReadOnlyDictionary<string, DeclarationName> declarationNames,
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
        else if (!TryMapFundamental(text, out kind))
        {
            if (!TryMapOpaque(text, declarationsByQualifiedName, declarationNames, out var pointee, out var wrapperType)
                || wrapperType is null)
            {
                return false;
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

        if (kind is AbiKind.StringView || type.Const || IsConstType(text))
        {
            return true;
        }

        if (!TryMapParameter(name, text, declarationsByQualifiedName, declarationNames, out var valueParameter))
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

        if (TryMapFundamental(text, out var kind))
        {
            parameter = new AbiParameter
            {
                Name = name,
                Kind = kind,
                NativeType = text,
            };
            return true;
        }

        if (TryMapOpaque(text, declarationsByQualifiedName, declarationNames, out var pointee, out var wrapperType)
            && wrapperType is not null)
        {
            parameter = new AbiParameter
            {
                Name = name,
                Kind = AbiKind.Opaque,
                NativeType = text,
                NativePointee = pointee,
                IsReference = IsReferenceType(text),
                IsConst = IsConstType(text),
                WrapperType = wrapperType,
            };
            return true;
        }

        return false;
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
        var text = canonical.Trim();
        return text.EndsWith("&", StringComparison.Ordinal)
            || text.EndsWith("*", StringComparison.Ordinal)
            || text.Contains("&", StringComparison.Ordinal)
            || text.Contains("*", StringComparison.Ordinal);
    }

    private static bool IsReferenceType(string canonical) =>
        canonical.TrimEnd().EndsWith("&", StringComparison.Ordinal);

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
        var text = canonical.Trim();
        while (text.EndsWith("&", StringComparison.Ordinal) || text.EndsWith("*", StringComparison.Ordinal))
        {
            text = text[..^1].TrimEnd();
        }

        if (text.StartsWith("const ", StringComparison.Ordinal))
        {
            text = text["const ".Length..].TrimStart();
        }

        if (text.EndsWith(" const", StringComparison.Ordinal))
        {
            text = text[..^" const".Length].TrimEnd();
        }

        return text;
    }
}
