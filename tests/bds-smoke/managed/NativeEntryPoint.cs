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

            var textA = a.toString_f7aee3c8();
            var textB = b.toString_f7aee3c8();
            var textC = c.toString_f7aee3c8();
            var seedA = a.randomSeed();
            var seedB = b.randomSeed();

            var fields = a.x == 1 && a.y == 2 && a.z == 3;
            a.x = 7;
            var textA2 = a.toString_f7aee3c8();

            using var moved = a.neighbor(0);
            var movedText = moved.toString_f7aee3c8();
            using var rebuilt = BlockPos.Create((float)moved.x, (float)moved.y, (float)moved.z);
            var rebuiltText = rebuilt.toString_f7aee3c8();

            var ok = textA.Length > 0
                && textA == textB
                && textA != textC
                && seedA == seedB
                && fields
                && textA2 != textA
                && movedText == rebuiltText;
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
