using Tokenometer.Models;

namespace Tokenometer.Services;

public interface IUsageSource
{
    string DisplayName { get; }

    Task<UsageSnapshot> GetSnapshotAsync(CancellationToken cancellationToken = default);
}

