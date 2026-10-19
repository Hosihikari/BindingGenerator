using Hosihikari.Bindings.Ir;

namespace Hosihikari.Bindings.Generator;

internal static class TypeNames
{
    private static readonly HashSet<string> Keywords = new(StringComparer.Ordinal)
    {
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked",
        "class", "const", "continue", "decimal", "default", "delegate", "do", "double", "else",
        "enum", "event", "explicit", "extern", "false", "finally", "fixed", "float", "for",
        "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal", "is", "lock",
        "long", "namespace", "new", "null", "object", "operator", "out", "override", "params",
        "private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed", "short",
        "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true",
        "try", "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort", "using", "virtual",
        "void", "volatile", "while",
    };

    public static string SurfaceNamespace(string surface) => surface switch
    {
        "bedrock" => "Bedrock",
        "levilamina" => "LeviLamina",
        "endstone" => "EndStone",
        _ => "Unknown",
    };

    public static string Escape(string name) => Keywords.Contains(name) ? "@" + name : name;

    public static string Sanitize(string name)
    {
        var builder = new System.Text.StringBuilder(name.Length);
        foreach (var character in name)
        {
            builder.Append(char.IsLetterOrDigit(character) || character == '_' ? character : '_');
        }

        if (builder.Length == 0)
        {
            builder.Append('_');
        }

        if (char.IsDigit(builder[0]))
        {
            builder.Insert(0, '_');
        }

        return Escape(builder.ToString());
    }

    public static string NativeNamespace(string qualifiedName)
    {
        var separator = qualifiedName.LastIndexOf("::", StringComparison.Ordinal);
        return separator < 0 ? "" : qualifiedName[..separator].Replace("::", ".", StringComparison.Ordinal);
    }

    public static string NativeName(string qualifiedName)
    {
        var separator = qualifiedName.LastIndexOf("::", StringComparison.Ordinal);
        return separator < 0 ? qualifiedName : qualifiedName[(separator + 2)..];
    }

    public static string ToCSharpType(TypeRef? type)
    {
        if (type is null)
        {
            return "nint";
        }

        var canonical = type.Canonical.Trim();
        return canonical switch
        {
            "void" => "void",
            "bool" => "bool",
            "char" or "signed char" or "unsigned char" or "int8_t" or "uint8_t" => "byte",
            "short" or "int16_t" => "short",
            "unsigned short" or "uint16_t" => "ushort",
            "int" or "int32_t" => "int",
            "unsigned int" or "uint32_t" => "uint",
            "long long" or "int64_t" => "long",
            "unsigned long long" or "uint64_t" => "ulong",
            "float" => "float",
            "double" => "double",
            "size_t" => "nuint",
            "long" => "nint",
            "unsigned long" => "nuint",
            _ => type.Kind == "fundamental" ? "nint" : "nint",
        };
    }
}
