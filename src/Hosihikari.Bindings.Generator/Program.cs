using Hosihikari.Bindings.Generator;
using Hosihikari.Bindings.Ir;

if (args.Length == 0 || args[0] is "-h" or "--help" or "help")
{
    PrintHelp();
    return args.Length == 0 ? 2 : 0;
}

switch (args[0])
{
    case "version":
        Console.WriteLine($"Hosihikari.Bindings.Generator 0.1.0 (IR {IrSchema.Version})");
        return 0;

    case "validate-ir":
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("usage: validate-ir <ir-manifest.json> <shard-directory>");
            return 2;
        }

        var manifest = ReadManifest(args[1]);
        var records = ReadRecords(args[2]);
        var result = IrValidator.Validate(manifest, records);
        if (!result.IsValid)
        {
            PrintErrors(result);
            return 1;
        }

        Console.WriteLine($"IR valid: {records.Count} records");
        return 0;
    }

    case "emit":
    {
        if (args.Length != 4)
        {
            Console.Error.WriteLine("usage: emit <ir-manifest.json> <shard-directory> <output-directory>");
            return 2;
        }

        var manifest = ReadManifest(args[1]);
        var records = ReadRecords(args[2]);
        var validation = IrValidator.Validate(manifest, records);
        if (!validation.IsValid)
        {
            PrintErrors(validation);
            return 1;
        }

        var manifestHash = "sha256:" + IrHash.CanonicalJsonSha256(File.ReadAllText(args[1]));
        var output = Path.GetFullPath(args[3]);
        foreach (var subdirectory in new[] { "interop", "shim", "coverage" })
        {
            var path = Path.Combine(output, subdirectory);
            if (Directory.Exists(path))
            {
                Directory.Delete(path, recursive: true);
            }
        }

        Directory.CreateDirectory(output);
        InteropEmitter.Emit(records, output);
        ShimEmitter.Emit(manifest, output, manifestHash);
        CoverageEmitter.Emit(manifest, records, output, manifestHash);
        Console.WriteLine($"emitted {records.Count} records to {output}");
        return 0;
    }

    case "validate-coverage":
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("usage: validate-coverage <coverage-summary.json> <coverage-manifest.jsonl>");
            return 2;
        }

        using var summaryStream = File.OpenRead(args[1]);
        var summary = IrJson.ReadCoverageSummary(summaryStream);
        using var entriesStream = File.OpenRead(args[2]);
        var entries = IrJson.ReadCoverageEntries(entriesStream);
        var result = IrValidator.ValidateCoverage(summary, entries);
        if (!result.IsValid)
        {
            PrintErrors(result);
            return 1;
        }

        Console.WriteLine($"coverage valid: {entries.Count} entries");
        return 0;
    }

    default:
        Console.Error.WriteLine($"unknown command: {args[0]}");
        PrintHelp();
        return 2;
}

static IrManifest ReadManifest(string path)
{
    using var stream = File.OpenRead(path);
    return IrJson.ReadManifest(stream);
}

static List<IrRecord> ReadRecords(string directory)
{
    var records = new List<IrRecord>();
    foreach (var file in Directory.EnumerateFiles(directory, "*.jsonl", SearchOption.AllDirectories)
                 .OrderBy(path => path, StringComparer.Ordinal))
    {
        using var stream = File.OpenRead(file);
        records.AddRange(IrJson.ReadRecords(stream));
    }

    return records;
}

static void PrintErrors(IrValidationResult result)
{
    Console.Error.WriteLine($"{result.Errors.Count} error(s):");
    foreach (var error in result.Errors)
    {
        Console.Error.WriteLine(" - " + error);
    }
}

static void PrintHelp()
{
    Console.WriteLine(
        """
        Hosihikari.Bindings.Generator

        Commands:
          version
          validate-ir <ir-manifest.json> <shard-directory>
          validate-coverage <coverage-summary.json> <coverage-manifest.jsonl>
          emit <ir-manifest.json> <shard-directory> <output-directory>
        """
    );
}
