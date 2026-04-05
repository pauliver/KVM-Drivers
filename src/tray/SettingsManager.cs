// SettingsManager.cs - Application settings persistence and auto-start
using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Win32;

namespace KVM.Tray
{
    // Application settings class
    public class AppSettings
    {
        // Network settings
        public int VncPort { get; set; } = 5900;
        public int WebSocketPort { get; set; } = 8443;
        public int HttpPort { get; set; } = 8080;  // HTTP port that serves index.html (web client)
        public bool VncRequireAuth { get; set; } = true;
        public string VncPassword { get; set; } = "";
        // UseTls = false: the WebSocket server runs plain ws:// by default.
        // The TLS infrastructure (tls_server.cpp) exists but is not yet wired.
        public bool UseTls { get; set; } = false;
        public string TlsCertificatePath { get; set; } = "";
        
        // Driver settings
        public bool AutoStartDrivers { get; set; } = false;
        public bool KeyboardEnabled { get; set; } = true;
        public bool MouseEnabled { get; set; } = true;
        public bool ControllerEnabled { get; set; } = false;
        public bool DisplayEnabled { get; set; } = false;
        
        // UI settings
        public bool MinimizeToTray { get; set; } = true;
        public bool StartMinimized { get; set; } = false;
        public bool AutoStartWithWindows { get; set; } = false;
        public int LogBufferSize { get; set; } = 1000;
        public string LastSelectedTab { get; set; } = "Drivers";
        
        // Video settings
        public int VideoResolutionWidth { get; set; } = 1920;
        public int VideoResolutionHeight { get; set; } = 1080;
        public int VideoFps { get; set; } = 60;
        public string VideoEncoder { get; set; } = "Auto";  // Auto, NVENC, AMF, QSV
        public int VideoBitrate { get; set; } = 10000;  // kbps
        
        // Connection limits
        public int VncMaxClients { get; set; } = 10;
        public int WsMaxClients  { get; set; } = 10;  // must be ≤ WS_MAX_CLIENTS (32) in websocket_server_async.cpp

        // VNC security
        public bool   VncAnonTls    { get; set; } = false;
        public string VncCertPin    { get; set; } = "";  // SHA-1 thumbprint for cert pinning

        // Global connection auth policy
        public bool   RequireRemoteAuth { get; set; } = true;  // Require auth for non-localhost
        public bool   TrustOnFirstUse   { get; set; } = true;  // Show approval dialog for unknowns
        public string AuthToken         { get; set; } = "";    // Pre-shared bearer token (optional)

        // Security settings
        public string[] AllowedIPs { get; set; } = new string[0];
        
        // Version for migrations
        public string SettingsVersion { get; set; } = "1.0";
    }

    public static class SettingsManager
    {
        private static readonly string SettingsFileName = "settings.json";
        private static readonly string AppName = "KVM-Drivers";
        
        // Settings cache
        private static AppSettings _cachedSettings;
        private static readonly object _lock = new object();
        
        // Get settings directory — uses CommonApplicationData (%PROGRAMDATA%) so that
        // settings.json is readable by KVMService which runs as NT AUTHORITY\LocalService.
        private static string GetSettingsDirectory()
        {
            string appData = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
            string settingsDir = Path.Combine(appData, AppName);
            
            if (!Directory.Exists(settingsDir))
            {
                Directory.CreateDirectory(settingsDir);
            }
            
            return settingsDir;
        }
        
        // Get full settings file path
        private static string GetSettingsPath()
        {
            return Path.Combine(GetSettingsDirectory(), SettingsFileName);
        }
        
        // Load settings from disk
        public static AppSettings LoadSettings()
        {
            lock (_lock)
            {
                if (_cachedSettings != null)
                {
                    return _cachedSettings;
                }
                
                string settingsPath = GetSettingsPath();
                
                if (File.Exists(settingsPath))
                {
                    try
                    {
                        string json = File.ReadAllText(settingsPath);
                        var options = new JsonSerializerOptions
                        {
                            PropertyNameCaseInsensitive = true,
                            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
                        };
                        
                        _cachedSettings = JsonSerializer.Deserialize<AppSettings>(json, options);
                        
                        // Ensure not null
                        if (_cachedSettings == null)
                        {
                            _cachedSettings = new AppSettings();
                        }
                        
                        return _cachedSettings;
                    }
                    catch (Exception ex)
                    {
                        System.Diagnostics.Debug.WriteLine($"Failed to load settings: {ex.Message}");
                    }
                }
                
                // Return default settings
                _cachedSettings = new AppSettings();
                return _cachedSettings;
            }
        }
        
        // Save settings to disk
        public static bool SaveSettings(AppSettings settings)
        {
            lock (_lock)
            {
                try
                {
                    string settingsPath = GetSettingsPath();
                    var options = new JsonSerializerOptions
                    {
                        WriteIndented = true,
                        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
                    };
                    
                    string json = JsonSerializer.Serialize(settings, options);
                    File.WriteAllText(settingsPath, json);
                    
                    _cachedSettings = settings;
                    return true;
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"Failed to save settings: {ex.Message}");
                    return false;
                }
            }
        }
        
        // Reset to defaults
        public static void ResetToDefaults()
        {
            lock (_lock)
            {
                _cachedSettings = new AppSettings();
                SaveSettings(_cachedSettings);
            }
        }
        
        // Auto-start with Windows
        public static bool SetAutoStartWithWindows(bool enable)
        {
            try
            {
                string runKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Run";
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(runKey, true))
                {
                    if (key != null)
                    {
                        if (enable)
                        {
                            string exePath = System.Reflection.Assembly.GetExecutingAssembly().Location;
                            key.SetValue(AppName, exePath);
                        }
                        else
                        {
                            key.DeleteValue(AppName, false);
                        }
                        return true;
                    }
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to set auto-start: {ex.Message}");
            }
            return false;
        }
        
        // Check if auto-start is enabled
        public static bool IsAutoStartEnabled()
        {
            try
            {
                string runKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Run";
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(runKey, false))
                {
                    if (key != null)
                    {
                        return key.GetValue(AppName) != null;
                    }
                }
            }
            catch { }
            return false;
        }
        
        // Export settings to file
        public static bool ExportSettings(string exportPath)
        {
            try
            {
                var settings = LoadSettings();
                var options = new JsonSerializerOptions
                {
                    WriteIndented = true
                };
                
                string json = JsonSerializer.Serialize(settings, options);
                File.WriteAllText(exportPath, json);
                return true;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to export settings: {ex.Message}");
                return false;
            }
        }
        
        // Import settings from file
        public static bool ImportSettings(string importPath)
        {
            try
            {
                if (File.Exists(importPath))
                {
                    string json = File.ReadAllText(importPath);
                    var options = new JsonSerializerOptions
                    {
                        PropertyNameCaseInsensitive = true
                    };
                    
                    var settings = JsonSerializer.Deserialize<AppSettings>(json, options);
                    if (settings != null)
                    {
                        return SaveSettings(settings);
                    }
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to import settings: {ex.Message}");
            }
            return false;
        }
        
        // Backup settings
        public static void BackupSettings()
        {
            try
            {
                string settingsPath = GetSettingsPath();
                if (File.Exists(settingsPath))
                {
                    string backupPath = settingsPath + ".backup";
                    File.Copy(settingsPath, backupPath, true);
                }
            }
            catch { }
        }
        
        // Restore from backup
        public static bool RestoreFromBackup()
        {
            try
            {
                string settingsPath = GetSettingsPath();
                string backupPath = settingsPath + ".backup";
                
                if (File.Exists(backupPath))
                {
                    File.Copy(backupPath, settingsPath, true);
                    _cachedSettings = null;  // Force reload
                    return true;
                }
            }
            catch { }
            return false;
        }
    }
}
