// MainWindow.xaml.cs - KVM Control Panel Code-Behind
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using System.ComponentModel;
using Microsoft.Win32;

namespace KVM.Tray
{
    public partial class MainWindow : Window
    {
        private ObservableCollection<ConnectionInfo> connections;
        private DispatcherTimer refreshTimer;
        private AppSettings settings;
        
        private ConnectionApprovalManager approvalManager_;

        public MainWindow()
        {
            InitializeComponent();
            LoadSettings();
            InitializeData();
            SetupTimer();
            ApplySettings();
            // Load persisted audit log
            DiagnosticsEngine.LoadAuditLog();
            AuditLogList.ItemsSource = DiagnosticsEngine.AuditLog;
            // Start connection approval manager (shows dialog for unknown remote clients)
            approvalManager_ = new ConnectionApprovalManager(Dispatcher);
            approvalManager_.ConnectionDecisionMade += (ip, result) =>
                AppendLog($"[Auth] {ip} → {result}");
            approvalManager_.Start();
            RefreshTrustedClientsList();
        }

        private void LoadSettings()
        {
            settings = SettingsManager.LoadSettings();
            
            // Check command line for minimized start
            string[] args = Environment.GetCommandLineArgs();
            foreach (string arg in args)
            {
                if (arg.Equals("--minimized", StringComparison.OrdinalIgnoreCase))
                {
                    settings.StartMinimized = true;
                    break;
                }
            }
        }

        private void ApplySettings()
        {
            // Apply network settings
            VncPort.Text = settings.VncPort.ToString();
            WsPort.Text = settings.WebSocketPort.ToString();
            VncRequireAuth.IsChecked = settings.VncRequireAuth;
            UseTls.IsChecked = settings.UseTls;

            // Apply IP allowlist
            if (settings.AllowedIPs != null && settings.AllowedIPs.Length > 0)
                IpAllowlist.Text = string.Join(Environment.NewLine, settings.AllowedIPs);

            // Apply max clients if controls exist
            if (settings.VncMaxClients > 0) VncMaxClients.Text = settings.VncMaxClients.ToString();
            if (settings.WsMaxClients  > 0) WsMaxClients.Text  = settings.WsMaxClients.ToString();

            // Apply VNC security settings
            VncAnonTls.IsChecked = settings.VncAnonTls;
            VncCertPin.Text = settings.VncCertPin ?? "";

            // Apply auth policy
            RequireRemoteAuth.IsChecked = settings.RequireRemoteAuth;
            TrustOnFirstUse.IsChecked   = settings.TrustOnFirstUse;
            AuthTokenBox.Text           = settings.AuthToken ?? "";
            
            // Apply driver auto-start if enabled
            if (settings.AutoStartDrivers)
            {
                AutoStartDrivers();
            }
            
            // Minimize if requested
            if (settings.StartMinimized)
            {
                WindowState = WindowState.Minimized;
                Hide();
            }
        }

        private void AutoStartDrivers()
        {
            // Auto-start enabled drivers
            if (settings.KeyboardEnabled) StartDriver("Keyboard", KeyboardStatus, KeyboardState, null);
            if (settings.MouseEnabled) StartDriver("Mouse", MouseStatus, MouseState, null);
            if (settings.ControllerEnabled) StartDriver("Controller", ControllerStatus, ControllerState, null);
            if (settings.DisplayEnabled) StartDriver("Display", DisplayStatus, DisplayState, null);
        }

        private void InitializeData()
        {
            connections = new ObservableCollection<ConnectionInfo>();
            ConnectionsList.ItemsSource = connections;
            
            // Add sample data for now
            connections.Add(new ConnectionInfo 
            { 
                ClientIP = "192.168.1.100", 
                ConnectionType = "VNC", 
                ConnectTime = DateTime.Now.ToString("HH:mm:ss") 
            });
        }

        private void SetupTimer()
        {
            refreshTimer = new DispatcherTimer();
            refreshTimer.Interval = TimeSpan.FromSeconds(2);
            refreshTimer.Tick += RefreshTimer_Tick;
            refreshTimer.Start();
        }

        private void RefreshTimer_Tick(object sender, EventArgs e)
        {
            UpdateDriverStatus();
        }

        private void UpdateDriverStatus()
        {
            // Check actual driver status via service or driver interface
            // For now, just UI refresh
        }

        // Driver Control Events
        private void KeyboardToggle_Click(object sender, RoutedEventArgs e)
        {
            ToggleDriver("Keyboard", KeyboardStatus, KeyboardState, (Button)sender);
        }

        private void MouseToggle_Click(object sender, RoutedEventArgs e)
        {
            ToggleDriver("Mouse", MouseStatus, MouseState, (Button)sender);
        }

        private void ControllerToggle_Click(object sender, RoutedEventArgs e)
        {
            ToggleDriver("Controller", ControllerStatus, ControllerState, (Button)sender);
        }

        private void DisplayToggle_Click(object sender, RoutedEventArgs e)
        {
            ToggleDriver("Display", DisplayStatus, DisplayState, (Button)sender);
        }

        private void ToggleDriver(string driverName, Ellipse statusIndicator, 
            TextBlock stateText, Button toggleButton)
        {
            bool isRunning = stateText.Text == "Running";
            
            if (isRunning)
            {
                StopDriver(driverName, statusIndicator, stateText, toggleButton);
            }
            else
            {
                StartDriver(driverName, statusIndicator, stateText, toggleButton);
            }
            
            // Save driver state to settings
            SaveDriverState(driverName, !isRunning);
        }
        
        private static string DriverServiceName(string driverName) => driverName switch
        {
            "Keyboard"   => "vhidkb",
            "Mouse"      => "vhidmouse",
            "Controller" => "vxinput",
            "Display"    => "vdisplay",
            _            => driverName
        };

        private void StartDriver(string driverName, Ellipse statusIndicator,
            TextBlock stateText, Button toggleButton)
        {
            string svcName = DriverServiceName(driverName);
            try
            {
                using var sc = new System.ServiceProcess.ServiceController(svcName);
                if (sc.Status != System.ServiceProcess.ServiceControllerStatus.Running)
                    sc.Start();
                sc.WaitForStatus(System.ServiceProcess.ServiceControllerStatus.Running,
                    TimeSpan.FromSeconds(5));
            }
            catch (Exception ex)
            {
                AppendLog($"[WARN] Could not start service '{svcName}': {ex.Message}");
            }

            if (statusIndicator != null) statusIndicator.Fill = Brushes.Green;
            if (stateText != null)       stateText.Text        = "Running";
            if (toggleButton != null)    toggleButton.Content  = "Stop";
            AppendLog($"Driver '{driverName}' started");
        }

        private void StopDriver(string driverName, Ellipse statusIndicator,
            TextBlock stateText, Button toggleButton)
        {
            string svcName = DriverServiceName(driverName);
            try
            {
                using var sc = new System.ServiceProcess.ServiceController(svcName);
                if (sc.Status == System.ServiceProcess.ServiceControllerStatus.Running)
                    sc.Stop();
                sc.WaitForStatus(System.ServiceProcess.ServiceControllerStatus.Stopped,
                    TimeSpan.FromSeconds(5));
            }
            catch (Exception ex)
            {
                AppendLog($"[WARN] Could not stop service '{svcName}': {ex.Message}");
            }

            if (statusIndicator != null) statusIndicator.Fill = Brushes.Gray;
            if (stateText != null)       stateText.Text        = "Stopped";
            if (toggleButton != null)    toggleButton.Content  = "Start";
            AppendLog($"Driver '{driverName}' stopped");
        }
        
        private void SaveDriverState(string driverName, bool running)
        {
            switch (driverName)
            {
                case "Keyboard": settings.KeyboardEnabled = running; break;
                case "Mouse": settings.MouseEnabled = running; break;
                case "Controller": settings.ControllerEnabled = running; break;
                case "Display": settings.DisplayEnabled = running; break;
            }
            SettingsManager.SaveSettings(settings);
        }

        // Log Events
        private void ClearLogs_Click(object sender, RoutedEventArgs e)
        {
            LogViewer.Clear();
        }

        private void ExportLogs_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                string logPath = System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "KVM-Drivers", "kvmlogs.txt");
                
                System.IO.File.WriteAllText(logPath, LogViewer.Text);
                MessageBox.Show($"Logs exported to {logPath}", "Export", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to export logs: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        public void AppendLog(string message)
        {
            string timestamp = DateTime.Now.ToString("HH:mm:ss");
            Dispatcher.Invoke(() =>
            {
                LogViewer.AppendText($"[{timestamp}] {message}\r\n");
                LogViewer.ScrollToEnd();
                
                // Limit log buffer
                if (LogViewer.Text.Length > 100000)
                {
                    LogViewer.Text = LogViewer.Text.Substring(LogViewer.Text.Length - 50000);
                }
            });
        }

        // Remote Events
        private void RestartServer_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                using var sc = new System.ServiceProcess.ServiceController("KVMService");
                if (sc.Status == System.ServiceProcess.ServiceControllerStatus.Running)
                {
                    sc.Stop();
                    sc.WaitForStatus(System.ServiceProcess.ServiceControllerStatus.Stopped,
                        TimeSpan.FromSeconds(10));
                }
                sc.Start();
                sc.WaitForStatus(System.ServiceProcess.ServiceControllerStatus.Running,
                    TimeSpan.FromSeconds(10));
                AppendLog("Remote server restarted");
            }
            catch (Exception ex)
            {
                AppendLog($"[ERROR] Failed to restart KVMService: {ex.Message}");
                MessageBox.Show($"Restart failed: {ex.Message}", "Error",
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void DisconnectClient_Click(object sender, RoutedEventArgs e)
        {
            var button = sender as Button;
            var connection = button?.DataContext as ConnectionInfo;
            if (connection != null)
            {
                connections.Remove(connection);
                AppendLog($"Disconnected client {connection.ClientIP}");
            }
        }

        // ── Trusted clients handlers ──────────────────────────────────────────
        private void RefreshTrustedClientsList()
        {
            var list = ConnectionApprovalManager.LoadTrustedClients();
            TrustedClientsList.ItemsSource = list;
        }

        private void RevokeTrusted_Click(object sender, RoutedEventArgs e)
        {
            var entry = TrustedClientsList.SelectedItem as TrustedClientEntry;
            if (entry == null) { MessageBox.Show("Select a client to revoke."); return; }
            ConnectionApprovalManager.RevokeTrustedClient(entry.IP);
            AppendLog($"[Auth] Revoked trust for {entry.IP}");
            RefreshTrustedClientsList();
        }

        private void RevokeAllTrusted_Click(object sender, RoutedEventArgs e)
        {
            if (MessageBox.Show("Revoke ALL trusted clients?", "Confirm",
                MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
            foreach (var entry in ConnectionApprovalManager.LoadTrustedClients())
                ConnectionApprovalManager.RevokeTrustedClient(entry.IP);
            AppendLog("[Auth] Revoked all trusted clients");
            RefreshTrustedClientsList();
        }

        private void RefreshTrusted_Click(object sender, RoutedEventArgs e)
            => RefreshTrustedClientsList();

        private void GenerateAuthToken_Click(object sender, RoutedEventArgs e)
        {
            var rng = System.Security.Cryptography.RandomNumberGenerator.Create();
            var bytes = new byte[24];
            rng.GetBytes(bytes);
            AuthTokenBox.Text = Convert.ToBase64String(bytes).TrimEnd('=');
            AppendLog("[Auth] New bearer token generated — save settings to apply");
        }

        // ── Diagnostics handlers ─────────────────────────────────────────────
        private async void RunDiagnostics_Click(object sender, RoutedEventArgs e)
        {
            DiagStatus.Text = "Running checks…";
            DiagResultsList.ItemsSource = null;

            var results = await Task.Run(() => DiagnosticsEngine.RunDriverHealthChecks());

            DiagResultsList.ItemsSource = results;

            int errors   = results.Count(r => r.Severity == DiagSeverity.Error);
            int warnings = results.Count(r => r.Severity == DiagSeverity.Warning);
            DiagStatus.Text = errors > 0
                ? $"{errors} error(s), {warnings} warning(s) — select a row and click Repair"
                : warnings > 0
                    ? $"All clear with {warnings} warning(s)"
                    : "All checks passed ✅";

            DiagnosticsEngine.LogAuditEvent("local", "Diagnostics",
                "HealthCheck", $"{results.Count} checks: {errors} errors, {warnings} warnings");
        }

        private async void RepairSelected_Click(object sender, RoutedEventArgs e)
        {
            var selected = DiagResultsList.SelectedItem as DiagResult;
            if (selected == null)
            {
                MessageBox.Show("Select a check result to repair.", "Repair",
                    MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            DiagStatus.Text = "Repairing…";
            var (success, msg) = await Task.Run(() => DiagnosticsEngine.AttemptRepair(selected));

            MessageBox.Show(msg, success ? "Repair Succeeded" : "Repair Failed",
                MessageBoxButton.OK,
                success ? MessageBoxImage.Information : MessageBoxImage.Warning);

            DiagStatus.Text = success ? "Repair complete — re-run checks" : "Repair failed";
            AppendLog($"Repair '{selected.Check}': {msg}");
        }

        private void ExportAuditLog_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new SaveFileDialog
            {
                Filter   = "CSV files (*.csv)|*.csv|All files (*.*)|*.*",
                FileName = $"kvm_audit_{DateTime.Now:yyyyMMdd_HHmmss}.csv",
                Title    = "Export Audit Log"
            };
            if (dlg.ShowDialog() == true)
            {
                bool ok = DiagnosticsEngine.ExportAuditLog(dlg.FileName);
                MessageBox.Show(ok ? $"Exported to {dlg.FileName}" : "Export failed.",
                    "Export Audit Log", MessageBoxButton.OK,
                    ok ? MessageBoxImage.Information : MessageBoxImage.Error);
            }
        }

        // ── Settings handlers ─────────────────────────────────────────────────
        private void SaveSettings_Click(object sender, RoutedEventArgs e)
        {
            // Update settings from UI
            if (int.TryParse(VncPort.Text, out int vncPort))
                settings.VncPort = vncPort;

            if (int.TryParse(WsPort.Text, out int wsPort))
                settings.WebSocketPort = wsPort;

            if (int.TryParse(VncMaxClients.Text, out int vncMax))
                settings.VncMaxClients = vncMax;

            if (int.TryParse(WsMaxClients.Text, out int wsMax))
                settings.WsMaxClients = wsMax;

            settings.VncRequireAuth = VncRequireAuth.IsChecked ?? true;
            settings.UseTls         = UseTls.IsChecked ?? true;
            settings.VncAnonTls       = VncAnonTls.IsChecked ?? false;
            settings.VncCertPin       = VncCertPin.Text?.Trim() ?? "";
            settings.RequireRemoteAuth = RequireRemoteAuth.IsChecked ?? true;
            settings.TrustOnFirstUse   = TrustOnFirstUse.IsChecked ?? true;
            settings.AuthToken         = AuthTokenBox.Text?.Trim() ?? "";

            // Parse IP allowlist (trim empty lines)
            settings.AllowedIPs = IpAllowlist.Text
                .Split(new[] { '\n', '\r' }, StringSplitOptions.RemoveEmptyEntries)
                .Select(s => s.Trim())
                .Where(s => s.Length > 0)
                .ToArray();

            settings.AutoStartWithWindows = AutoStartCheckBox?.IsChecked ?? false;
            settings.MinimizeToTray = MinimizeToTrayCheckBox?.IsChecked ?? true;
            
            // Save to disk
            if (SettingsManager.SaveSettings(settings))
            {
                // Set auto-start registry
                SettingsManager.SetAutoStartWithWindows(settings.AutoStartWithWindows);
                
                MessageBox.Show("Settings saved successfully", "Save", MessageBoxButton.OK, MessageBoxImage.Information);
                AppendLog("Settings saved");
            }
            else
            {
                MessageBox.Show("Failed to save settings", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void ResetSettings_Click(object sender, RoutedEventArgs e)
        {
            if (MessageBox.Show("Reset all settings to defaults?", "Confirm", 
                MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            {
                SettingsManager.ResetToDefaults();
                settings = SettingsManager.LoadSettings();
                ApplySettings();
                AppendLog("Settings reset to defaults");
            }
        }

        private void ImportSettings_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Filter = "JSON files (*.json)|*.json|All files (*.*)|*.*",
                Title = "Import Settings"
            };
            
            if (dialog.ShowDialog() == true)
            {
                if (SettingsManager.ImportSettings(dialog.FileName))
                {
                    settings = SettingsManager.LoadSettings();
                    ApplySettings();
                    MessageBox.Show("Settings imported successfully", "Import", MessageBoxButton.OK, MessageBoxImage.Information);
                    AppendLog("Settings imported");
                }
                else
                {
                    MessageBox.Show("Failed to import settings", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }

        private void ExportSettings_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new Microsoft.Win32.SaveFileDialog
            {
                Filter = "JSON files (*.json)|*.json|All files (*.*)|*.*",
                Title = "Export Settings",
                FileName = "kvm-settings.json"
            };
            
            if (dialog.ShowDialog() == true)
            {
                if (SettingsManager.ExportSettings(dialog.FileName))
                {
                    MessageBox.Show("Settings exported successfully", "Export", MessageBoxButton.OK, MessageBoxImage.Information);
                    AppendLog("Settings exported");
                }
                else
                {
                    MessageBox.Show("Failed to export settings", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }

        protected override void OnClosing(CancelEventArgs e)
        {
            refreshTimer?.Stop();
            approvalManager_?.Stop();
            base.OnClosing(e);
        }
        
        protected override void OnStateChanged(EventArgs e)
        {
            if (WindowState == WindowState.Minimized && settings.MinimizeToTray)
            {
                Hide();
            }
            base.OnStateChanged(e);
        }
    }

    public class ConnectionInfo
    {
        public string ClientIP { get; set; }
        public string ConnectionType { get; set; }
        public string ConnectTime { get; set; }
    }

    // Null-safe CheckBox helper referenced before UI is fully loaded
    internal static class CheckBoxExt
    {
        public static bool IsCheckedSafe(this CheckBox cb, bool defaultVal = false)
            => cb?.IsChecked ?? defaultVal;
    }
}
