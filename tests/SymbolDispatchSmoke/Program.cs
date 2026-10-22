using System.Runtime.InteropServices;
using Hosihikari.Bindings.Bedrock.Interop;

if (args.Length != 2)
{
    Console.Error.WriteLine("usage: SymbolDispatchSmoke <fixture-module> <dispatch-shim>");
    return 2;
}

NativeLibrary.Load(Path.GetFullPath(args[0]));
NativeMethods.Load(args[1]);

var block = FixtureBlock.create(2, 3)
    ?? throw new InvalidOperationException("FixtureBlock.create returned null.");

if (block.add(4) != 9)
{
    throw new InvalidOperationException("RVA dispatch returned an unexpected value.");
}

if (block.isReady() != 1)
{
    throw new InvalidOperationException("The first vtable slot returned an unexpected value.");
}

if (block.scale(5) != 25)
{
    throw new InvalidOperationException("The second vtable slot returned an unexpected value.");
}

FixtureBlock.release(block);
FixtureBlock.release(null);

Console.WriteLine("Symbol dispatch smoke test passed.");
return 0;
