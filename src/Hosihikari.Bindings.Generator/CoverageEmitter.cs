using System.Text;
using Hosihikari.Bindings.Ir;

namespace Hosihikari.Bindings.Generator;

internal static class CoverageEmitter
{
    public static void Emit(IrManifest manifest, IReadOnlyList<IrRecord> records, string outputDirectory, string manifestHash)
    {
        var declarations = records.OfType<DeclarationRecord>()
            .OrderBy(declaration => declaration.Id, StringComparer.Ordinal)
            .ToList();
        var members = records.OfType<MemberRecord>()
            .GroupBy(member => (member.DeclarationId, member.Configuration))
            .ToDictionary(
                group => group.Key,
                group => group.OrderBy(member => member.Id, StringComparer.Ordinal).ToList());

        var configurations = manifest.Configurations
            .OrderBy(configuration => configuration.Id, StringComparer.Ordinal)
            .ToList();

        var entries = new List<CoverageEntry>();
        foreach (var declaration in declarations)
        {
            foreach (var configuration in configurations)
            {
                entries.Add(new CoverageEntry
                {
                    Id = declaration.Id,
                    Parent = null,
                    Surface = declaration.Surface,
                    Configuration = configuration.Id,
                    Kind = declaration.Kind,
                    QualifiedName = declaration.QualifiedName,
                    Status = CoverageStatus.Bound,
                    Emitters = ["interop"],
                    Source = declaration.Source,
                });

                if (!members.TryGetValue((declaration.Id, configuration.Id), out var declarationMembers))
                {
                    continue;
                }

                foreach (var member in declarationMembers)
                {
                    entries.Add(new CoverageEntry
                    {
                        Id = member.Id,
                        Parent = declaration.Id,
                        Surface = declaration.Surface,
                        Configuration = configuration.Id,
                        Kind = member.Kind,
                        QualifiedName = declaration.QualifiedName + "::" + member.Name,
                        Status = CoverageStatus.Bound,
                        Emitters = ["interop"],
                        Source = declaration.Source,
                    });
                }
            }
        }

        var coverageDirectory = Path.Combine(outputDirectory, "coverage");
        Directory.CreateDirectory(coverageDirectory);
        var coveragePath = Path.Combine(coverageDirectory, "manifest.jsonl");
        using (var stream = File.Create(coveragePath))
        {
            IrJson.WriteCoverageEntries(stream, entries);
        }

        var manifestSha256 = "sha256:" + IrHash.Sha256Hex(File.ReadAllBytes(coveragePath));
        var summary = new CoverageSummary
        {
            IrVersion = manifest.IrVersion,
            IrManifestSha256 = manifestHash,
            Configurations = configurations.Select(configuration => configuration.Id).ToList(),
            Declarations = Count(entries.Where(entry => entry.Parent is null)),
            Members = Count(entries.Where(entry => entry.Parent is not null)),
            BySurface = entries
                .GroupBy(entry => entry.Surface, StringComparer.Ordinal)
                .OrderBy(group => group.Key, StringComparer.Ordinal)
                .ToDictionary(group => group.Key, group => Count(group), StringComparer.Ordinal),
            ManifestSha256 = manifestSha256,
        };

        using (var stream = File.Create(Path.Combine(coverageDirectory, "summary.json")))
        {
            IrJson.WriteCoverageSummary(stream, summary);
        }

        var validation = IrValidator.ValidateCoverage(summary, entries);
        if (!validation.IsValid)
        {
            throw new InvalidOperationException(
                "Emitted coverage failed validation: " + string.Join(" | ", validation.Errors));
        }
    }

    private static CoverageCounts Count(IEnumerable<CoverageEntry> entries)
    {
        var list = entries.ToList();
        return new CoverageCounts
        {
            Total = list.Count,
            Bound = list.Count(entry => entry.Status == CoverageStatus.Bound),
            Skipped = list.Count(entry => entry.Status == CoverageStatus.Skipped),
            Excluded = list.Count(entry => entry.Status == CoverageStatus.Excluded),
            Unknown = list.Count(entry => entry.Status == CoverageStatus.Unknown),
        };
    }
}
