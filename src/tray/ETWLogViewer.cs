// ETWLogViewer.cs - Windows Event Tracing for Drivers log viewer
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Parsers;
using Microsoft.Diagnostics.Tracing.Session;

namespace KVM.Tray
{
    /// <summary>
    /// ETW (Event Tracing for Windows) log viewer for driver diagnostics
    /// Captures real-time events from vhidkb, vhidmouse, vxinput, vdisplay drivers
    /// </summary>
    public class ETWLogViewer : IDisposable
    {
        private TraceEventSession _session;
        private BackgroundWorker _worker;
        private bool _isRunning;
        private List<LogEntry> _logBuffer;
        private readonly object _lock = new object();
        
        // Driver provider GUIDs (must match driver WPP tracing)
        private static readonly Guid VhidkbProviderGuid = new Guid("A1B2C3D4-E5F6-7890-ABCD-EF1234567890");
        private static readonly Guid VhidmouseProviderGuid = new Guid("B2C3D4E5-F6A7-8901-BCDE-F23456789012");
        private static readonly Guid VxinputProviderGuid = new Guid("C3D4E5F6-A7B8-9012-CDEF-345678901234");
        private static readonly Guid VdisplayProviderGuid = new Guid("D4E5F6A7-B8C9-0123-DEFA-456789012345");
        
        public event EventHandler<LogEntry> LogEntryReceived;
        public event EventHandler<string> SessionError;
        
        public bool IsRunning => _isRunning;
        
        public class LogEntry
        {
            public DateTime Timestamp { get; set; }
            public string Provider { get; set; }
            public TraceEventLevel Level { get; set; }
            public string Message { get; set; }
            public string DriverName { get; set; }
            public int ProcessId { get; set; }
            public int ThreadId { get; set; }
        }
        
        public ETWLogViewer()
        {
            _logBuffer = new List<LogEntry>();
        }
        
        /// <summary>
        /// Start ETW session and capture driver events
        /// </summary>
        public void StartSession()
        {
            if (_isRunning) return;
            
            try
            {
                // Create a new ETW session with a unique name
                string sessionName = $"KVM-Drivers-{Guid.NewGuid()}";
                _session = new TraceEventSession(sessionName);
                
                // Enable driver providers
                EnableDriverProviders();
                
                // Setup background worker for async processing
                _worker = new BackgroundWorker { WorkerSupportsCancellation = true };
                _worker.DoWork += Worker_DoWork;
                _worker.RunWorkerCompleted += Worker_RunWorkerCompleted;
                _worker.RunWorkerAsync();
                
                _isRunning = true;
                Debug.WriteLine($"[ETW] Session started: {sessionName}");
            }
            catch (Exception ex)
            {
                SessionError?.Invoke(this, $"Failed to start ETW session: {ex.Message}");
            }
        }
        
        /// <summary>
        /// Stop ETW session
        /// </summary>
        public void StopSession()
        {
            _isRunning = false;
            _worker?.CancelAsync();
            _session?.Dispose();
            _session = null;
            Debug.WriteLine("[ETW] Session stopped");
        }
        
        /// <summary>
        /// Get recent log entries from buffer
        /// </summary>
        public List<LogEntry> GetRecentEntries(int count = 100)
        {
            lock (_lock)
            {
                return _logBuffer
                    .OrderByDescending(e => e.Timestamp)
                    .Take(count)
                    .OrderBy(e => e.Timestamp)
                    .ToList();
            }
        }
        
        /// <summary>
        /// Filter log entries by driver, level, or search term
        /// </summary>
        public List<LogEntry> FilterEntries(string driverName = null, 
            TraceEventLevel? minLevel = null, string searchTerm = null)
        {
            IEnumerable<LogEntry> query = _logBuffer;
            
            if (!string.IsNullOrEmpty(driverName))
                query = query.Where(e => e.DriverName?.Equals(driverName, StringComparison.OrdinalIgnoreCase) == true);
            
            if (minLevel.HasValue)
                query = query.Where(e => e.Level <= minLevel.Value);
            
            if (!string.IsNullOrEmpty(searchTerm))
                query = query.Where(e => e.Message?.Contains(searchTerm, StringComparison.OrdinalIgnoreCase) == true);
            
            return query.OrderByDescending(e => e.Timestamp).ToList();
        }
        
        /// <summary>
        /// Export logs to file
        /// </summary>
        public void ExportToFile(string path, bool includeAll = false)
        {
            var entries = includeAll ? _logBuffer : GetRecentEntries(1000);
            
            var sb = new StringBuilder();
            sb.AppendLine("Timestamp,Level,Driver,Message,PID,TID");
            
            foreach (var entry in entries.OrderBy(e => e.Timestamp))
            {
                sb.AppendLine($"{entry.Timestamp:yyyy-MM-dd HH:mm:ss.fff},{entry.Level},{entry.DriverName},\"{entry.Message}\",{entry.ProcessId},{entry.ThreadId}");
            }
            
            File.WriteAllText(path, sb.ToString());
        }
        
        /// <summary>
        /// Clear log buffer
        /// </summary>
        public void ClearBuffer()
        {
            lock (_lock)
            {
                _logBuffer.Clear();
            }
        }
        
        /// <summary>
        /// Get driver statistics from logs
        /// </summary>
        public Dictionary<string, DriverStats> GetDriverStatistics(TimeSpan? timeWindow = null)
        {
            var cutoff = timeWindow.HasValue ? DateTime.Now - timeWindow.Value : DateTime.MinValue;
            
            var stats = _logBuffer
                .Where(e => e.Timestamp >= cutoff)
                .GroupBy(e => e.DriverName ?? "Unknown")
                .ToDictionary(
                    g => g.Key,
                    g => new DriverStats
                    {
                        TotalEvents = g.Count(),
                        Errors = g.Count(e => e.Level <= TraceEventLevel.Error),
                        Warnings = g.Count(e => e.Level == TraceEventLevel.Warning),
                        LastActivity = g.Max(e => e.Timestamp)
                    }
                );
            
            return stats;
        }
        
        private void EnableDriverProviders()
        {
            // Enable our driver providers with appropriate levels
            _session.EnableProvider(VhidkbProviderGuid, TraceEventLevel.Verbose, 0xFFFFFFFFFFFFFFFF);
            _session.EnableProvider(VhidmouseProviderGuid, TraceEventLevel.Verbose, 0xFFFFFFFFFFFFFFFF);
            _session.EnableProvider(VxinputProviderGuid, TraceEventLevel.Verbose, 0xFFFFFFFFFFFFFFFF);
            _session.EnableProvider(VdisplayProviderGuid, TraceEventLevel.Verbose, 0xFFFFFFFFFFFFFFFF);
            
            // Also enable generic Windows driver events
            _session.EnableKernelProvider(KernelTraceEventParser.Keywords.DiskIO | 
                KernelTraceEventParser.Keywords.NetworkTCPIP);
        }
        
        private void Worker_DoWork(object sender, DoWorkEventArgs e)
        {
            using (var source = new ETWTraceEventSource(_session.Name, TraceEventSourceType.Session))
            {
                source.Dynamic.All += (eventData) =>
                {
                    if (_worker.CancellationPending)
                    {
                        e.Cancel = true;
                        return;
                    }
                    
                    // Skip if not from our drivers
                    if (!IsDriverEvent(eventData)) return;
                    
                    var entry = new LogEntry
                    {
                        Timestamp = DateTime.Now,
                        Provider = eventData.ProviderName,
                        Level = eventData.Level,
                        Message = eventData.FormattedMessage ?? eventData.ToString(),
                        DriverName = GetDriverName(eventData.ProviderGuid),
                        ProcessId = eventData.ProcessID,
                        ThreadId = eventData.ThreadID
                    };
                    
                    lock (_lock)
                    {
                        _logBuffer.Add(entry);
                        
                        // Keep buffer size manageable
                        if (_logBuffer.Count > 10000)
                            _logBuffer.RemoveAt(0);
                    }
                    
                    // Raise event on UI thread
                    Application.Current?.Dispatcher?.BeginInvoke(DispatcherPriority.Background, 
                        new Action(() => LogEntryReceived?.Invoke(this, entry)));
                };
                
                // Process events until cancelled
                source.Process();
            }
        }
        
        private void Worker_RunWorkerCompleted(object sender, RunWorkerCompletedEventArgs e)
        {
            if (e.Error != null)
            {
                SessionError?.Invoke(this, $"ETW processing error: {e.Error.Message}");
            }
        }
        
        private bool IsDriverEvent(TraceEvent eventData)
        {
            return eventData.ProviderGuid == VhidkbProviderGuid ||
                   eventData.ProviderGuid == VhidmouseProviderGuid ||
                   eventData.ProviderGuid == VxinputProviderGuid ||
                   eventData.ProviderGuid == VdisplayProviderGuid;
        }
        
        private string GetDriverName(Guid providerGuid)
        {
            if (providerGuid == VhidkbProviderGuid) return "vhidkb";
            if (providerGuid == VhidmouseProviderGuid) return "vhidmouse";
            if (providerGuid == VxinputProviderGuid) return "vxinput";
            if (providerGuid == VdisplayProviderGuid) return "vdisplay";
            return null;
        }
        
        public void Dispose()
        {
            StopSession();
            _session?.Dispose();
            _worker?.Dispose();
        }
    }
    
    public class DriverStats
    {
        public int TotalEvents { get; set; }
        public int Errors { get; set; }
        public int Warnings { get; set; }
        public DateTime LastActivity { get; set; }
        
        public bool IsHealthy => Errors == 0;
        public string Status => IsHealthy ? "Healthy" : (Errors > 10 ? "Critical" : "Warning");
    }
}
