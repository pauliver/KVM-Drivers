// DiagnosticsEngine.cs - Driver health checks, self-repair, and audit logging
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace KVM.Tray
{
    // Severity level for diagnostic results
    public enum DiagSeverity { Ok, Warning, Error }

    // A single diagnostic check result
    public class DiagResult
    {
        public string Check { get; set; }
        public DiagSeverity Severity { get; set; }
        public string Message { get; set; }
        public string RepairAction { get; set; }  // null = not auto-repairable
        public DateTime Timestamp { get; set; } = DateTime.Now;

        public string SeverityLabel => Severity.ToString().ToUpper();
        public string Icon => Severity switch
        {
            DiagSeverity.Ok      => "✅",
            DiagSeverity.Warning => "⚠️",
            DiagSeverity.Error   => "❌",
            _                    => "?"
        };
    }

    // A per-connection audit log entry
    public class AuditEntry
    {
        public DateTime Timestamp { get; set; }
        public string ClientIP { get; set; }
        public string Protocol { get; set; }      // VNC, WebSocket, Local
        public string EventType { get; set; }     // Connected, Disconnected, AuthFailed, InputInjected
        public string Details { get; set; }

        public string TimestampStr => Timestamp.ToString("yyyy-MM-dd HH:mm:ss");
    }

    public static class DiagnosticsEngine
    {
        private static readonly string[] KnownDriverNames =
        {
            "vhidkb", "vhidmouse", "vxinput", "vdisplay"
        };

        private static readonly ObservableCollection<AuditEntry> auditLog =
            new ObservableCollection<AuditEntry>();

        public static ObservableCollection<AuditEntry> AuditLog => auditLog;

        // Run all driver health checks and return results
        public static List<DiagResult> RunDriverHealthChecks()
        {
            var results = new List<DiagResult>();

            // Check each driver's device file presence
            foreach (var driver in KnownDriverNames)
            {
                bool accessible = IsDriverAccessible(driver);
                results.Add(new DiagResult
                {
                    Check = $"Driver: {driver}",
                    Severity = accessible ? DiagSeverity.Ok : DiagSeverity.Warning,
                    Message = accessible
                        ? $"{driver} device accessible"
                        : $"{driver} device not found — driver may not be installed",
                    RepairAction = accessible ? null : $"install:{driver}"
                });
            }

            // Check for WDF runtime
            results.Add(CheckWdfRuntime());

            // Check Winsock
            results.Add(CheckWinsock());

            // Check port availability
            results.Add(CheckPortAvailability(5900, "VNC"));
            results.Add(CheckPortAvailability(8443, "WebSocket"));

            // Check disk space for log files
            results.Add(CheckDiskSpace());

            // Check for pending system reboots (may affect driver loading)
            results.Add(CheckPendingReboot());

            return results;
        }

        // Attempt auto-repair for a result that has a RepairAction
        public static (bool success, string message) AttemptRepair(DiagResult result)
        {
            if (result.RepairAction == null)
                return (false, "No repair action available");

            if (result.RepairAction.StartsWith("install:"))
            {
                string driverName = result.RepairAction.Substring(8);
                return RepairDriverInstallation(driverName);
            }

            return (false, $"Unknown repair action: {result.RepairAction}");
        }

        // Log a connection/input event to the audit trail
        public static void LogAuditEvent(string clientIP, string protocol,
            string eventType, string details = "")
        {
            var entry = new AuditEntry
            {
                Timestamp = DateTime.Now,
                ClientIP  = clientIP ?? "local",
                Protocol  = protocol,
                EventType = eventType,
                Details   = details
            };

            // Add to in-memory log (UI-thread safe via dispatcher if needed)
            lock (auditLog)
            {
                if (auditLog.Count >= 10000)
                {
                    auditLog.RemoveAt(0);
                }
                auditLog.Add(entry);
            }

            // Persist to audit log file
            PersistAuditEntry(entry);
        }

        // Export audit log to CSV
        public static bool ExportAuditLog(string filePath)
        {
            try
            {
                var sb = new StringBuilder();
                sb.AppendLine("Timestamp,ClientIP,Protocol,EventType,Details");
                lock (auditLog)
                {
                    foreach (var entry in auditLog)
                    {
                        sb.AppendLine(
                            $"\"{entry.TimestampStr}\"," +
                            $"\"{entry.ClientIP}\"," +
                            $"\"{entry.Protocol}\"," +
                            $"\"{entry.EventType}\"," +
                            $"\"{entry.Details.Replace("\"", "\"\"")}\"");
                    }
                }
                File.WriteAllText(filePath, sb.ToString(), Encoding.UTF8);
                return true;
            }
            catch
            {
                return false;
            }
        }

        // Load audit log from disk on startup
        public static void LoadAuditLog()
        {
            string path = GetAuditLogPath();
            if (!File.Exists(path)) return;

            try
            {
                var lines = File.ReadLines(path).Skip(1);  // skip CSV header
                foreach (var line in lines.TakeLast(5000))  // cap at 5000 entries
                {
                    var cols = ParseCsvLine(line);
                    if (cols.Length >= 5)
                    {
                        auditLog.Add(new AuditEntry
                        {
                            Timestamp = DateTime.TryParse(cols[0], out var dt) ? dt : DateTime.Now,
                            ClientIP  = cols[1],
                            Protocol  = cols[2],
                            EventType = cols[3],
                            Details   = cols[4]
                        });
                    }
                }
            }
            catch { /* ignore corrupt log */ }
        }

        // --- Private helpers ---

        [DllImport("kernel32.dll", CharSet = CharSet.Auto)]
        private static extern IntPtr CreateFile(string lpFileName, uint dwAccess,
            uint dwShareMode, IntPtr lpSecA, uint dwCreation, uint dwFlags, IntPtr hTemplate);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr hObject);

        private static bool IsDriverAccessible(string driverName)
        {
            string devicePath = $"\\\\.\\{driverName}";
            IntPtr handle = CreateFile(devicePath, 0x80000000 /* GENERIC_READ */,
                1 /* FILE_SHARE_READ */, IntPtr.Zero, 3 /* OPEN_EXISTING */,
                0, IntPtr.Zero);

            if (handle == IntPtr.Zero || handle == new IntPtr(-1))
                return false;

            CloseHandle(handle);
            return true;
        }

        private static DiagResult CheckWdfRuntime()
        {
            bool wdfPresent = File.Exists(
                Path.Combine(Environment.SystemDirectory, "drivers", "Wdf01000.sys"));
            return new DiagResult
            {
                Check    = "WDF Runtime",
                Severity = wdfPresent ? DiagSeverity.Ok : DiagSeverity.Error,
                Message  = wdfPresent ? "WDF runtime present" : "WDF runtime (Wdf01000.sys) not found — install WDF from Windows Update",
                RepairAction = null
            };
        }

        private static DiagResult CheckWinsock()
        {
            // Simple check: winsock key exists
            bool wsOk = Registry.LocalMachine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Services\WinSock2\Parameters") != null;
            return new DiagResult
            {
                Check    = "Winsock",
                Severity = wsOk ? DiagSeverity.Ok : DiagSeverity.Error,
                Message  = wsOk ? "Winsock2 configured" : "Winsock2 registry key missing — run 'netsh winsock reset'",
            };
        }

        private static DiagResult CheckPortAvailability(int port, string name)
        {
            bool inUse = IsPortInUse(port);
            return new DiagResult
            {
                Check    = $"Port {port} ({name})",
                Severity = inUse ? DiagSeverity.Warning : DiagSeverity.Ok,
                Message  = inUse
                    ? $"Port {port} is already in use — {name} server may fail to bind"
                    : $"Port {port} available for {name}",
            };
        }

        private static bool IsPortInUse(int port)
        {
            try
            {
                var listener = new System.Net.Sockets.TcpListener(
                    System.Net.IPAddress.Loopback, port);
                listener.Start();
                listener.Stop();
                return false;
            }
            catch
            {
                return true;
            }
        }

        private static DiagResult CheckDiskSpace()
        {
            try
            {
                var drive = new DriveInfo(
                    Path.GetPathRoot(Environment.GetFolderPath(
                        Environment.SpecialFolder.LocalApplicationData)));
                long freeGb = drive.AvailableFreeSpace / (1024 * 1024 * 1024);
                return new DiagResult
                {
                    Check    = "Disk Space (Logs)",
                    Severity = freeGb < 1 ? DiagSeverity.Warning : DiagSeverity.Ok,
                    Message  = $"{freeGb} GB free on {drive.Name}" +
                               (freeGb < 1 ? " — low disk space may cause log loss" : ""),
                };
            }
            catch (Exception ex)
            {
                return new DiagResult
                {
                    Check    = "Disk Space",
                    Severity = DiagSeverity.Warning,
                    Message  = $"Could not check disk space: {ex.Message}",
                };
            }
        }

        private static DiagResult CheckPendingReboot()
        {
            bool pendingReboot = IsPendingReboot();
            return new DiagResult
            {
                Check    = "Pending Reboot",
                Severity = pendingReboot ? DiagSeverity.Warning : DiagSeverity.Ok,
                Message  = pendingReboot
                    ? "System has a pending reboot — drivers may not load correctly until rebooted"
                    : "No pending reboot",
            };
        }

        private static bool IsPendingReboot()
        {
            const string cbsKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending";
            const string wuKey  = @"SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired";
            return Registry.LocalMachine.OpenSubKey(cbsKey) != null ||
                   Registry.LocalMachine.OpenSubKey(wuKey)  != null;
        }

        private static (bool success, string message) RepairDriverInstallation(string driverName)
        {
            string infPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory,
                "drivers", $"{driverName}.inf");

            if (!File.Exists(infPath))
                return (false, $"Driver INF not found: {infPath}");

            try
            {
                var psi = new ProcessStartInfo("pnputil.exe",
                    $"/add-driver \"{infPath}\" /install")
                {
                    Verb            = "runas",
                    UseShellExecute = true,
                    CreateNoWindow  = true
                };
                var proc = Process.Start(psi);
                proc?.WaitForExit(30000);
                bool ok = proc?.ExitCode == 0;
                return (ok, ok ? $"{driverName} installed successfully"
                               : $"pnputil returned exit code {proc?.ExitCode}");
            }
            catch (Exception ex)
            {
                return (false, $"Repair failed: {ex.Message}");
            }
        }

        private static string GetAuditLogPath()
        {
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "KVM-Drivers");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "audit_log.csv");
        }

        private static void PersistAuditEntry(AuditEntry entry)
        {
            try
            {
                string path = GetAuditLogPath();
                bool exists = File.Exists(path);
                using var writer = new StreamWriter(path, append: true, Encoding.UTF8);
                if (!exists)
                    writer.WriteLine("Timestamp,ClientIP,Protocol,EventType,Details");
                writer.WriteLine(
                    $"\"{entry.TimestampStr}\"," +
                    $"\"{entry.ClientIP}\"," +
                    $"\"{entry.Protocol}\"," +
                    $"\"{entry.EventType}\"," +
                    $"\"{entry.Details.Replace("\"", "\"\"")}\"");
            }
            catch { /* non-fatal */ }
        }

        private static string[] ParseCsvLine(string line)
        {
            var parts = new List<string>();
            bool inQuotes = false;
            var current = new StringBuilder();
            foreach (char c in line)
            {
                if (c == '"')       { inQuotes = !inQuotes; }
                else if (c == ',' && !inQuotes) { parts.Add(current.ToString()); current.Clear(); }
                else                { current.Append(c); }
            }
            parts.Add(current.ToString());
            return parts.ToArray();
        }
    }
}
