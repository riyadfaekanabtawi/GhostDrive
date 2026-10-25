using System.Text.Json;

namespace GhostDriveApp.Core;

public enum PeerConnection
{
    Direct,
    Relayed,
    Offline,
}

public sealed record PeerStatus(string Name, string Id, string? Address, bool Online, bool Relayed)
{
    public PeerConnection Connection =>
        !Online ? PeerConnection.Offline : Relayed ? PeerConnection.Relayed : PeerConnection.Direct;

    public string ConnectionText => Connection switch
    {
        PeerConnection.Direct => "Online",
        PeerConnection.Relayed => "Online, relayed",
        _ => "Offline",
    };
}

public sealed record RendezvousStatus(string Address, bool Connected, string? Observed);

// The output of `ghostdrive status --json` (daemon/src/status_json.hpp). Parse cleans every
// string meant for display; Mountpoint is kept as given, since it is used to open the drive,
// and shown through DisplayText.Clean.
public sealed record DriveStatus(
    string Device,
    string Id,
    string? Mountpoint,
    int Port,
    RendezvousStatus? Rendezvous,
    string? RemovedBy,
    string? DiscoveryError,
    IReadOnlyList<PeerStatus> Peers)
{
    // Null when the text is not the status object.
    public static DriveStatus? Parse(string? json)
    {
        if (string.IsNullOrWhiteSpace(json)) return null;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return null;
            if (Text(root, "device") is not { } device || Text(root, "id") is not { } id) return null;
            if (!root.TryGetProperty("peers", out var peersElement) ||
                peersElement.ValueKind != JsonValueKind.Array)
            {
                return null;
            }

            var peers = new List<PeerStatus>();
            foreach (var p in peersElement.EnumerateArray())
            {
                if (p.ValueKind != JsonValueKind.Object) return null;
                if (Text(p, "name") is not { } name || Text(p, "id") is not { } peerId) return null;
                peers.Add(new PeerStatus(DisplayText.Clean(name), DisplayText.Clean(peerId, 128),
                    Clean(Text(p, "address")), Flag(p, "online"), Flag(p, "relayed")));
            }

            RendezvousStatus? rendezvous = null;
            if (root.TryGetProperty("rendezvous", out var rv) && rv.ValueKind == JsonValueKind.Object)
            {
                rendezvous = new RendezvousStatus(DisplayText.Clean(Text(rv, "address")),
                    Flag(rv, "connected"), Clean(Text(rv, "observed")));
            }

            var port = root.TryGetProperty("port", out var portElement) &&
                       portElement.TryGetInt32(out var n) ? n : 0;
            var mountpoint = Text(root, "mountpoint");
            return new DriveStatus(DisplayText.Clean(device), DisplayText.Clean(id, 128),
                string.IsNullOrEmpty(mountpoint) ? null : mountpoint, port, rendezvous,
                Clean(Text(root, "removed_by")), Clean(Text(root, "discovery_error"), 160), peers);
        }
        catch (JsonException)
        {
            return null;
        }
    }

    // Value equality including the peer list, so the UI redraws only when something changed.
    public bool SameAs(DriveStatus? other) =>
        other is not null && this with { Peers = other.Peers } == other &&
        Peers.SequenceEqual(other.Peers);

    private static string? Text(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private static bool Flag(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;

    private static string? Clean(string? text, int max = DisplayText.MaxLength) =>
        text is null ? null : DisplayText.Clean(text, max);
}
