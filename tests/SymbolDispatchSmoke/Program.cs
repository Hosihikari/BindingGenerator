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

if (block.label() != "5")
{
    throw new InvalidOperationException("The by-value string return is incorrect.");
}

if (!block.numbers().SequenceEqual([2, 3]))
{
    throw new InvalidOperationException("The by-value int vector return is incorrect.");
}

// FixturePair has a user-provided copy constructor but a trivial destructor, so it is passed
// by hidden reference even though it is only 8 bytes.
using (var pair = block.pair())
{
    if (block.sumPair(pair) != 7)
    {
        throw new InvalidOperationException("The trivially destructible by-value record is incorrect.");
    }
}

// The checked-in Linux fixture IR covers the member string/vector returns above. The PDB-derived
// Windows IR additionally contains the parameter and static cases; reflect so both IRs share
// this program.
if (typeof(FixtureBlock).GetMethod("names") is not null)
{
    if (!Call<string[]>(block, "names").SequenceEqual(["2", "3"]))
    {
        throw new InvalidOperationException("The by-value string vector return is incorrect.");
    }

    if (!Call<int[]>(block, "squares", 3).SequenceEqual([0, 1, 4]))
    {
        throw new InvalidOperationException("The static int vector return is incorrect.");
    }

    if (Call<int>(block, "countNames", (object)new[] { "a", "b" }) != 4)
    {
        throw new InvalidOperationException("The string vector parameter is incorrect.");
    }

    if (Call<int>(block, "sumValues", (object)new[] { 1, 2, 3 }) != 9)
    {
        throw new InvalidOperationException("The int vector parameter is incorrect.");
    }
}

FixtureBlock.release(block);
FixtureBlock.release(null);

Console.WriteLine("Symbol dispatch smoke test passed.");
return 0;

static T Call<T>(FixtureBlock target, string name, params object?[] arguments)
{
    var method = typeof(FixtureBlock).GetMethod(name)
        ?? throw new InvalidOperationException($"FixtureBlock.{name} is unavailable.");
    return (T)method.Invoke(method.IsStatic ? null : target, arguments)!;
}
