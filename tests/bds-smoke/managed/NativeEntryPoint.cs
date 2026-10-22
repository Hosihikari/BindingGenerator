using System.Runtime.InteropServices;
using Hosihikari.Bindings.Bedrock.Interop;

namespace Hosihikari.SymbolSmoke;

/// <summary>Entry point resolved through <c>hostfxr</c> by the preload-native plugin.</summary>
public static class NativeEntryPoint
{
    [UnmanagedCallersOnly]
    public static int Run(IntPtr argument, int argumentLength)
    {
        var resultPath = Environment.GetEnvironmentVariable("HS_SYMBOL_RESULT");
        try
        {
            var shim = Environment.GetEnvironmentVariable("HS_SYMBOL_SHIM")
                ?? throw new InvalidOperationException("HS_SYMBOL_SHIM is not set.");
            NativeMethods.Load(shim);

            using var a = BlockPos.Create(1.0f, 2.0f, 3.0f);
            using var b = BlockPos.Create(1.0f, 2.0f, 3.0f);
            using var c = BlockPos.Create(1.0f, 2.0f, 4.0f);

            // The hash suffix on toString depends on the IR; resolve it by shape instead.
            var toString = typeof(BlockPos).GetMethods()
                .First(method => method.Name.StartsWith("toString", StringComparison.Ordinal)
                    && method.GetParameters().Length == 0
                    && method.ReturnType == typeof(string));
            string Text(BlockPos value) => (string)toString.Invoke(value, null)!;

            var textA = Text(a);
            var textB = Text(b);
            var textC = Text(c);
            var seedA = a.randomSeed();
            var seedB = b.randomSeed();

            var fields = a.x == 1 && a.y == 2 && a.z == 3;
            a.x = 7;
            var textA2 = Text(a);

            using var moved = a.neighbor(0);
            var movedText = Text(moved);
            using var rebuilt = BlockPos.Create((float)moved.x, (float)moved.y, (float)moved.z);
            var rebuiltText = Text(rebuilt);

            using var origin = BlockPos.Create(17.0f, 34.0f, 51.0f);
            using var chunk = ChunkPos.Create(origin);
            using var vector = chunk.toBlockSpaceVec3(64.0f);
            var vectorLength = vector.length();
            var expectedLength = MathF.Sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
            using var normalized = vector.normalized();
            var normalizedLength = normalized.length();
            using var absolute = vector.abs();
            var absoluteLength = absolute.length();

            var ok = textA.Length > 0
                && textA == textB
                && textA != textC
                && seedA == seedB
                && fields
                && textA2 != textA
                && movedText == rebuiltText
                && chunk.x == 1
                && vectorLength > 0.0f
                && MathF.Abs(vectorLength - expectedLength) < 0.01f
                && MathF.Abs(normalizedLength - 1.0f) < 0.01f
                && MathF.Abs(absoluteLength - vectorLength) < 0.01f;
            var result =
                "load=managed\n"
                + $"a={textA}\n"
                + $"b={textB}\n"
                + $"c={textC}\n"
                + $"a2={textA2}\n"
                + $"moved={movedText}\n"
                + $"rebuilt={rebuiltText}\n"
                + $"seedA={seedA}\n"
                + $"seedB={seedB}\n"
                + $"chunk=({chunk.x},{chunk.y},{chunk.z})\n"
                + $"vec=({vector.x},{vector.y},{vector.z})\n"
                + $"length={vectorLength}\n"
                + $"normalized={normalizedLength}\n"
                + $"absolute={absoluteLength}\n"
                + $"result={(ok ? "OK" : "FAIL")}\n";
            if (resultPath is not null)
            {
                File.WriteAllText(resultPath, result);
            }

            return ok ? 0 : 1;
        }
        catch (Exception exception)
        {
            if (resultPath is not null)
            {
                try
                {
                    File.WriteAllText(resultPath, "load=failed\nresult=FAIL\n" + exception + Environment.NewLine);
                }
                catch (IOException)
                {
                }
            }

            return 1;
        }
    }
}
