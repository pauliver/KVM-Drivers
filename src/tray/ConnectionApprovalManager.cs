// ConnectionApprovalManager.cs - Polls the pending_approvals directory and shows
// the ConnectionApprovalDialog for each inbound connection request from C++ servers.
// Writes result files back so the server can unblock and allow/reject the connection.

using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;

namespace KVM.Tray
{
    public class ApprovalRequest
    {
        public string Id { get; set; }
        public string ClientIP { get; set; }
        public string Protocol { get; set; }
        public string Timestamp { get; set; }
        public bool   IsAuthenticated { get; set; }
    }

    public class ConnectionApprovalManager : IDisposable
    {
        private static readonly string PendingDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "KVM-Drivers", "pending_approvals");

        private static readonly string TrustedClientsFile = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "KVM-Drivers", "trusted_clients.txt");

        private CancellationTokenSource _cts;
        private Task _pollTask;
        private readonly Dispatcher _dispatcher;
        private readonly HashSet<string> _processingIds = new HashSet<string>();
        private readonly object _lock = new object();

        // Raised on the UI thread when a connection is approved/rejected/blocked
        public event Action<string, string> ConnectionDecisionMade;  // (clientIP, result)

        public ConnectionApprovalManager(Dispatcher dispatcher)
        {
            _dispatcher = dispatcher;
        }

        public void Start()
        {
            Directory.CreateDirectory(PendingDir);
            _cts = new CancellationTokenSource();
            _pollTask = Task.Run(() => PollLoop(_cts.Token));
        }

        public void Stop()
        {
            _cts?.Cancel();
            try { _pollTask?.Wait(TimeSpan.FromSeconds(2)); } catch { }
        }

        public void Dispose() => Stop();

        // Read the trusted clients file and return current list
        public static List<TrustedClientEntry> LoadTrustedClients()
        {
            var list = new List<TrustedClientEntry>();
            if (!File.Exists(TrustedClientsFile)) return list;

            foreach (var line in File.ReadLines(TrustedClientsFile))
            {
                var parts = line.Trim().Split(new[] { ' ' }, 2);
                if (parts.Length >= 1 && !string.IsNullOrEmpty(parts[0]))
                {
                    list.Add(new TrustedClientEntry
                    {
                        IP = parts[0],
                        ExpiryEpoch = parts.Length > 1 && long.TryParse(parts[1], out long e) ? e : 0
                    });
                }
            }
            return list;
        }

        // Write an approved/blocked client to the trusted clients file
        public static void WriteTrustedClient(string ip, int durationMinutes = 0)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(TrustedClientsFile));
            long expiry = 0;
            if (durationMinutes > 0)
            {
                // Windows FILETIME epoch: 100-nanosecond intervals since 1601-01-01
                expiry = DateTime.UtcNow.AddMinutes(durationMinutes)
                    .ToFileTimeUtc();
            }
            var lines = File.Exists(TrustedClientsFile)
                ? new List<string>(File.ReadAllLines(TrustedClientsFile))
                : new List<string>();

            // Replace or add
            bool found = false;
            for (int i = 0; i < lines.Count; i++)
            {
                if (lines[i].StartsWith(ip + " ") || lines[i] == ip)
                {
                    lines[i] = $"{ip} {expiry}";
                    found = true;
                    break;
                }
            }
            if (!found) lines.Add($"{ip} {expiry}");
            File.WriteAllLines(TrustedClientsFile, lines);
        }

        public static void RevokeTrustedClient(string ip)
        {
            if (!File.Exists(TrustedClientsFile)) return;
            var lines = new List<string>(File.ReadAllLines(TrustedClientsFile));
            lines.RemoveAll(l => l.StartsWith(ip + " ") || l == ip);
            File.WriteAllLines(TrustedClientsFile, lines);
        }

        // ── Private ──────────────────────────────────────────────────────────

        private async Task PollLoop(CancellationToken ct)
        {
            while (!ct.IsCancellationRequested)
            {
                try
                {
                    foreach (var file in Directory.GetFiles(PendingDir, "*.request"))
                    {
                        string id = Path.GetFileNameWithoutExtension(file);
                        bool alreadyProcessing;
                        lock (_lock) { alreadyProcessing = _processingIds.Contains(id); }
                        if (alreadyProcessing) continue;

                        lock (_lock) { _processingIds.Add(id); }
                        var request = ParseRequestFile(file);
                        if (request != null)
                        {
                            // Show dialog on UI thread — do not await here, fire independently
                            _ = _dispatcher.InvokeAsync(() => ShowApprovalDialog(request));
                        }
                    }
                }
                catch (Exception ex)
                {
                    // Non-fatal: log and continue polling
                    System.Diagnostics.Debug.WriteLine($"[ApprovalManager] Poll error: {ex.Message}");
                }

                await Task.Delay(500, ct).ConfigureAwait(false);
            }
        }

        private void ShowApprovalDialog(ApprovalRequest request)
        {
            var dlgRequest = new ConnectionApprovalDialog.ConnectionRequest
            {
                ClientIP         = request.ClientIP,
                Hostname         = TryResolveHostname(request.ClientIP),
                Protocol         = request.Protocol,
                RequestedAccess  = "Control",
                RequestTime      = DateTime.TryParse(request.Timestamp, out var dt) ? dt : DateTime.Now,
                IsAuthenticated  = request.IsAuthenticated
            };

            var dlg = new ConnectionApprovalDialog(dlgRequest);
            // Make dialog show on top of everything
            dlg.Topmost = true;
            dlg.ShowInTaskbar = true;
            bool? result = dlg.ShowDialog();

            string decision;
            int durationMinutes = dlg.ApprovedDurationMinutes;

            switch (dlg.Result)
            {
                case ConnectionApprovalDialog.ConnectionResult.Approved:
                    decision = "approved";
                    WriteTrustedClient(request.ClientIP, durationMinutes);
                    DiagnosticsEngine.LogAuditEvent(request.ClientIP, request.Protocol,
                        "Approved", $"duration={durationMinutes}min");
                    break;

                case ConnectionApprovalDialog.ConnectionResult.BlockedPermanently:
                    decision = "blocked";
                    // Add to blocked list (negative expiry = blocked)
                    DiagnosticsEngine.LogAuditEvent(request.ClientIP, request.Protocol,
                        "BlockedPermanently", "");
                    break;

                case ConnectionApprovalDialog.ConnectionResult.TimedOut:
                    decision = "timedout";
                    DiagnosticsEngine.LogAuditEvent(request.ClientIP, request.Protocol,
                        "ApprovalTimedOut", "auto-rejected after 30s");
                    break;

                default:
                    decision = "rejected";
                    DiagnosticsEngine.LogAuditEvent(request.ClientIP, request.Protocol,
                        "Rejected", "user declined");
                    break;
            }

            // Write result file so C++ server can unblock
            WriteResultFile(request.Id, decision);
            lock (_lock) { _processingIds.Remove(request.Id); }

            ConnectionDecisionMade?.Invoke(request.ClientIP, decision);
        }

        private static void WriteResultFile(string id, string result)
        {
            string path = Path.Combine(PendingDir, id + ".result");
            File.WriteAllText(path, result);
        }

        private static ApprovalRequest ParseRequestFile(string filePath)
        {
            try
            {
                var req = new ApprovalRequest
                {
                    Id = Path.GetFileNameWithoutExtension(filePath)
                };
                foreach (var line in File.ReadAllLines(filePath))
                {
                    var idx = line.IndexOf('=');
                    if (idx < 0) continue;
                    var key = line.Substring(0, idx);
                    var val = line.Substring(idx + 1);
                    switch (key)
                    {
                        case "ip":            req.ClientIP       = val; break;
                        case "protocol":      req.Protocol       = val; break;
                        case "timestamp":     req.Timestamp      = val; break;
                        case "authenticated": req.IsAuthenticated = val == "1"; break;
                    }
                }
                return string.IsNullOrEmpty(req.ClientIP) ? null : req;
            }
            catch { return null; }
        }

        private static string TryResolveHostname(string ip)
        {
            try
            {
                var entry = System.Net.Dns.GetHostEntry(ip);
                return entry.HostName != ip ? entry.HostName : null;
            }
            catch { return null; }
        }
    }

    public class TrustedClientEntry
    {
        public string IP { get; set; }
        public long   ExpiryEpoch { get; set; }  // 0 = permanent

        public string ExpiryDisplay
        {
            get
            {
                if (ExpiryEpoch == 0) return "Permanent";
                try
                {
                    var dt = DateTime.FromFileTimeUtc(ExpiryEpoch).ToLocalTime();
                    return dt < DateTime.Now ? "Expired" : dt.ToString("yyyy-MM-dd HH:mm");
                }
                catch { return "Unknown"; }
            }
        }

        public bool IsExpired => ExpiryEpoch != 0 &&
            DateTime.FromFileTimeUtc(ExpiryEpoch).ToLocalTime() < DateTime.Now;
    }
}
