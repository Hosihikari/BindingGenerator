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
        Console.WriteLine($"Hosihikari.Bindings.Generator 0.2.0 (IR {IrSchema.Version})");
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
        if (args.Length < 4)
        {
            Console.Error.WriteLine(
                "usage: emit <ir-manifest.json> <shard-directory> <output-directory> [--native-root <path>] [--link-carrier]");
            return 2;
        }

        string? nativeRoot = null;
        var linkCarrier = false;
        for (var index = 4; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--native-root" when index + 1 < args.Length:
                    nativeRoot = Path.GetFullPath(args[++index]);
                    break;
                case "--link-carrier":
                    linkCarrier = true;
                    break;
                default:
                    Console.Error.WriteLine($"unknown option: {args[index]}");
                    return 2;
            }
        }

        if (linkCarrier && nativeRoot is null)
        {
            Console.Error.WriteLine("--link-carrier requires --native-root.");
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
        var plan = AbiPlan.Build(records, nativeRoot, linkCarrier);
        foreach (var subdirectory in new[] { "interop", "shim", "coverage" })
        {
            DeleteDirectory(Path.Combine(output, subdirectory));
        }

        Directory.CreateDirectory(output);
        InteropEmitter.Emit(records, output, plan);
        ShimEmitter.Emit(manifest, records, output, manifestHash, plan);
        CoverageEmitter.Emit(manifest, records, output, manifestHash, plan);
        Console.WriteLine(
            $"emitted {records.Count} records to {output} ({plan.Functions.Count} BindingShim functions)");
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

static void DeleteDirectory(string path)
{
    for (var attempt = 0; attempt < 3; attempt++)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, recursive: true);
            }

            return;
        }
        catch (IOException) when (attempt < 2)
        {
            Thread.Sleep(100);
        }
        catch (UnauthorizedAccessException) when (attempt < 2)
        {
            Thread.Sleep(100);
        }
    }
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
