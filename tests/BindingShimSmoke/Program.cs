using System.Text;
using Hosihikari.Bindings.LeviLamina.Interop;
using Hosihikari.Bindings.LeviLamina.Interop.ll.@event;
using Hosihikari.Bindings.LeviLamina.Interop.ll.hash_utils;

if (args.Length != 1)
{
    Console.Error.WriteLine("usage: Smoke <path-to-shim.dll>");
    return 2;
}

NativeMethods.Load(args[0]);

const string text = "hosihikari";
using var id = EventId.Create(text);
if (id.name != text)
{
    throw new InvalidOperationException($"EventId.name was '{id.name}'.");
}

var baseView = new HashedIdBase(id.Handle);
var expected = Fnv1a(text);
if (baseView.hash != expected)
{
    throw new InvalidOperationException($"EventId.hash was 0x{baseView.hash:x16}, expected 0x{expected:x16}.");
}

if (!baseView.operator_equal(new HashedIdBase(id.Handle)))
{
    throw new InvalidOperationException("HashedIdBase::operator== returned false for equal hashes.");
}

using var combiner = HashCombiner.Create(123);
if (combiner.Handle == 0)
{
    throw new InvalidOperationException("HashCombiner.Create returned a null handle.");
}

if (combiner.hash() != 123 || combiner.operator_unsigned_long_long() != 123)
{
    throw new InvalidOperationException("HashCombiner method calls returned an unexpected seed.");
}

Console.WriteLine($"name={id.name} hash=0x{baseView.hash:x16} expected=0x{expected:x16}");
Console.WriteLine("BindingShim smoke test passed.");
return 0;

static ulong Fnv1a(string value)
{
    var hash = 0xcbf29ce484222325UL;
    foreach (var b in Encoding.UTF8.GetBytes(value))
    {
        hash ^= b;
        hash *= 0x100000001b3UL;
    }

    return hash;
}
