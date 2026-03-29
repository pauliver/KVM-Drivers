# MemoryLeakAudit.ps1 - Comprehensive memory leak detection for KVM-Drivers
# Scans codebase for common memory leak patterns in kernel and user-mode code

param(
    [string]$SourcePath = "..\src",
    [string]$OutputFile = "memory_audit_report.txt",
    [switch]$FixIssues,
    [switch]$Verbose
)

$ErrorActionPreference = "Continue"

# Patterns that indicate potential memory leaks
$LeakPatterns = @{
    # Kernel-mode patterns
    Kernel = @{
        ExAllocatePool_NoFree = @{
            Pattern = 'ExAllocatePool(?:WithTag)?\s*\([^)]+\)(?!.*ExFreePool)'
            Description = "ExAllocatePool without matching ExFreePool"
            Severity = "High"
        }
        
        ExAllocatePool_MissingTag = @{
            Pattern = 'ExAllocatePool\s*\([^)]+\)(?!WithTag)'
            Description = "ExAllocatePool without pool tag (use ExAllocatePoolWithTag)"
            Severity = "Medium"
        }
        
        ZwCreateFile_NoClose = @{
            Pattern = 'ZwCreateFile\s*\([^)]+\)(?!.*ZwClose)'
            Description = "ZwCreateFile handle may not be closed"
            Severity = "Medium"
        }
        
        IoCreateDevice_NoDelete = @{
            Pattern = 'IoCreateDevice\s*\([^)]+\)(?!.*IoDeleteDevice)'
            Description = "IoCreateDevice without IoDeleteDevice"
            Severity = "High"
        }
        
        ObReferenceObject_NoDereference = @{
            Pattern = 'ObReferenceObjectBy[^{]+{(?!.*ObDereferenceObject)'
            Description = "ObReferenceObject without ObDereferenceObject"
            Severity = "High"
        }
        
        MmMapLockedPages_NoUnmap = @{
            Pattern = 'MmMapLockedPages(?:SpecifyCache)?\s*\([^)]+\)(?!.*MmUnmapLockedPages)'
            Description = "MmMapLockedPages without MmUnmapLockedPages"
            Severity = "High"
        }
        
        PsCreateSystemThread_NoClose = @{
            Pattern = 'PsCreateSystemThread\s*\([^)]+\)(?!.*ZwClose)'
            Description = "PsCreateSystemThread handle not closed"
            Severity = "Medium"
        }
        
        ExInitializeResource_NoDelete = @{
            Pattern = 'ExInitializeResource(?:Lite)?\s*\([^)]+\)(?!.*ExDeleteResource(?:Lite)?)'
            Description = "Resource initialized but not deleted"
            Severity = "Medium"
        }
        
        EtwRegister_NoUnregister = @{
            Pattern = 'EtwRegister\s*\([^)]+\)(?!.*EtwUnregister)'
            Description = "ETW provider registered but not unregistered"
            Severity = "Low"
        }
    }
    
    # User-mode patterns
    Usermode = @{
        Malloc_NoFree = @{
            Pattern = '(?:malloc|calloc|realloc)\s*\([^)]+\)(?!.*free\s*\()'
            Description = "malloc/calloc without free"
            Severity = "High"
        }
        
        New_NoDelete = @{
            Pattern = 'new\s+(?!\[)(?!.*delete\s)(?!.*std::)'
            Description = "new without delete"
            Severity = "High"
        }
        
        NewArray_NoDeleteArray = @{
            Pattern = 'new\s*\[[^\]]+\](?!.*delete\s*\[)'
            Description = "new[] without delete[]"
            Severity = "High"
        }
        
        CreateFile_NoCloseHandle = @{
            Pattern = 'CreateFile[AW]?\s*\([^)]+\)(?!.*CloseHandle)'
            Description = "CreateFile handle not closed"
            Severity = "Medium"
        }
        
        GlobalAlloc_NoFree = @{
            Pattern = 'GlobalAlloc\s*\([^)]+\)(?!.*GlobalFree)'
            Description = "GlobalAlloc without GlobalFree"
            Severity = "Medium"
        }
        
        LocalAlloc_NoFree = @{
            Pattern = 'LocalAlloc\s*\([^)]+\)(?!.*LocalFree)'
            Description = "LocalAlloc without LocalFree"
            Severity = "Medium"
        }
        
        CoTaskMemAlloc_NoFree = @{
            Pattern = 'CoTaskMemAlloc\s*\([^)]+\)(?!.*CoTaskMemFree)'
            Description = "CoTaskMemAlloc without CoTaskMemFree"
            Severity = "Medium"
        }
        
        HeapAlloc_NoFree = @{
            Pattern = 'HeapAlloc\s*\([^)]+\)(?!.*HeapFree)'
            Description = "HeapAlloc without HeapFree"
            Severity = "High"
        }
        
        VirtualAlloc_NoFree = @{
            Pattern = 'VirtualAlloc\s*\([^)]+\)(?!.*VirtualFree)'
            Description = "VirtualAlloc without VirtualFree"
            Severity = "High"
        }
        
        MapViewOfFile_NoUnmap = @{
            Pattern = 'MapViewOfFile(?:Ex)?\s*\([^)]+\)(?!.*UnmapViewOfFile)'
            Description = "MapViewOfFile without UnmapViewOfFile"
            Severity = "Medium"
        }
        
        CreateFileMapping_NoClose = @{
            Pattern = 'CreateFileMapping[AW]?\s*\([^)]+\)(?!.*CloseHandle)'
            Description = "CreateFileMapping handle not closed"
            Severity = "Medium"
        }
        
        WSAStartup_NoCleanup = @{
            Pattern = 'WSAStartup\s*\([^)]+\)(?!.*WSACleanup)'
            Description = "WSAStartup without WSACleanup"
            Severity = "Medium"
        }
        
        socket_NoClosesocket = @{
            Pattern = 'socket\s*\([^)]+\)(?!.*closesocket)'
            Description = "socket without closesocket"
            Severity = "Medium"
        }
        
        CreateThread_NoCloseHandle = @{
            Pattern = 'CreateThread\s*\([^)]+\)(?!.*CloseHandle)'
            Description = "CreateThread handle not closed"
            Severity = "Low"
        }
        
        LoadLibrary_NoFreeLibrary = @{
            Pattern = 'LoadLibrary[AWEx]?\s*\([^)]+\)(?!.*FreeLibrary)'
            Description = "LoadLibrary without FreeLibrary"
            Severity = "Low"
        }
    }
    
    # Common patterns (both kernel and user-mode)
    Common = @{
        D3D11CreateDevice_NoRelease = @{
            Pattern = 'D3D11CreateDevice\s*\([^)]+\)(?!.*Release\s*\()'
            Description = "D3D11 device created but not released"
            Severity = "High"
        }
        
        IDXGIFactory_CreateSwapChain_NoRelease = @{
            Pattern = 'CreateSwapChain\s*\([^)]+\)(?!.*Release\s*\()'
            Description = "Swap chain created but not released"
            Severity = "High"
        }
        
        CreateDXGIFactory_NoRelease = @{
            Pattern = 'CreateDXGIFactory\s*\([^)]+\)(?!.*Release\s*\()'
            Description = "DXGI factory created but not released"
            Severity = "Medium"
        }
        
        DirectXResource_NoRelease = @{
            Pattern = '(?:Get|Create)[A-Za-z]+\s*\([^)]+\)(?:\s*->\s*)?QueryInterface\s*\([^)]+\)(?!.*Release\s*\()'
            Description = "DirectX interface obtained but not released"
            Severity = "High"
        }
    }
}

# Code review patterns (not necessarily leaks, but suspicious)
$ReviewPatterns = @{
    EarlyReturn_NoCleanup = @{
        Pattern = '(?:return|goto)\s+\w+;(?![^}]*free|[^}]*delete|[^}]*ExFreePool|[^}.*CloseHandle|[^}.*ZwClose)'
        Description = "Early return/goto without cleanup - review needed"
        Severity = "Review"
    }
    
    ErrorPath_Cleanup = @{
        Pattern = 'if\s*\(\s*!NT_SUCCESS|if\s*\(\s*FAILED|if\s*\(\s*.*==\s*NULL\s*\)(?!.*{.*cleanup|[^}]*goto)'
        Description = "Error path may not cleanup properly"
        Severity = "Review"
    }
    
    Exception_NoFinally = @{
        Pattern = '__try\s*{(?!.*__finally)'
        Description = "__try without __finally - ensure cleanup"
        Severity = "Review"
    }
}

# Statistics
$Stats = @{
    FilesScanned = 0
    LinesScanned = 0
    IssuesFound = 0
    HighSeverity = 0
    MediumSeverity = 0
    LowSeverity = 0
    KernelIssues = 0
    UsermodeIssues = 0
}

# Results storage
$Results = @()

function Write-Log {
    param([string]$Message, [string]$Level = "INFO")
    $timestamp = Get-Date -Format "HH:mm:ss"
    $line = "[$timestamp] [$Level] $Message"
    
    switch ($Level) {
        "ERROR" { Write-Host $line -ForegroundColor Red }
        "WARN"  { Write-Host $line -ForegroundColor Yellow }
        "SUCCESS" { Write-Host $line -ForegroundColor Green }
        default { Write-Host $line }
    }
    
    Add-Content -Path $OutputFile -Value $line -ErrorAction SilentlyContinue
}

function Scan-File {
    param([string]$FilePath)
    
    $content = Get-Content -Path $FilePath -Raw -ErrorAction SilentlyContinue
    if (-not $content) { return }
    
    $Stats.FilesScanned++
    $Stats.LinesScanned += ($content -split "`n").Count
    
    $isKernel = $FilePath -match "drivers[\\/]"
    $fileType = if ($isKernel) { "Kernel" } else { "Usermode" }
    
    if ($Verbose) {
        Write-Log "Scanning $fileType file: $FilePath"
    }
    
    # Check patterns based on file type
    $patternsToCheck = @()
    
    if ($isKernel) {
        $patternsToCheck += $LeakPatterns.Kernel
    } else {
        $patternsToCheck += $LeakPatterns.Usermode
    }
    $patternsToCheck += $LeakPatterns.Common
    $patternsToCheck += $ReviewPatterns
    
    foreach ($category in $patternsToCheck.Keys) {
        $patterns = $patternsToCheck[$category]
        
        foreach ($patternName in $patterns.Keys) {
            $pattern = $patterns[$patternName]
            
            $matches = [regex]::Matches($content, $pattern.Pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
            
            foreach ($match in $matches) {
                # Get line number
                $lineNum = ($content.Substring(0, $match.Index) -split "`n").Count
                
                # Get context (surrounding lines)
                $lines = $content -split "`n"
                $contextStart = [Math]::Max(0, $lineNum - 3)
                $contextEnd = [Math]::Min($lines.Count - 1, $lineNum + 2)
                $context = $lines[$contextStart..$contextEnd] -join "`n"
                
                $issue = [PSCustomObject]@{
                    File = $FilePath
                    Line = $lineNum
                    Category = $category
                    Pattern = $patternName
                    Description = $pattern.Description
                    Severity = $pattern.Severity
                    Context = $context.Trim()
                    MatchText = $match.Value
                }
                
                $Results += $issue
                $Stats.IssuesFound++
                
                switch ($issue.Severity) {
                    "High" { $Stats.HighSeverity++ }
                    "Medium" { $Stats.MediumSeverity++ }
                    "Low" { $Stats.LowSeverity++ }
                }
                
                if ($isKernel) {
                    $Stats.KernelIssues++
                } else {
                    $Stats.UsermodeIssues++
                }
            }
        }
    }
}

function Generate-Report {
    $report = @"
================================================================================
           KVM-DRIVERS MEMORY LEAK AUDIT REPORT
================================================================================
Generated: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss")
Source Path: $(Resolve-Path $SourcePath)
Files Scanned: $($Stats.FilesScanned)
Lines of Code: $($Stats.LinesScanned)

SUMMARY
-------
Total Issues Found: $($Stats.IssuesFound)
  - High Severity: $($Stats.HighSeverity)
  - Medium Severity: $($Stats.MediumSeverity)
  - Low Severity: $($Stats.LowSeverity)
  - Review Items: $(($Results | Where-Object { $_.Severity -eq "Review" }).Count)

Kernel-Mode Issues: $($Stats.KernelIssues)
User-Mode Issues: $($Stats.UsermodeIssues)

DETAILED FINDINGS
-----------------

"@

    # Group by severity
    $highIssues = $Results | Where-Object { $_.Severity -eq "High" } | Sort-Object File, Line
    $mediumIssues = $Results | Where-Object { $_.Severity -eq "Medium" } | Sort-Object File, Line
    $lowIssues = $Results | Where-Object { $_.Severity -eq "Low" } | Sort-Object File, Line
    $reviewItems = $Results | Where-Object { $_.Severity -eq "Review" } | Sort-Object File, Line

    if ($highIssues) {
        $report += "`n=== HIGH SEVERITY (Potential Memory Leaks) ===`n"
        foreach ($issue in $highIssues) {
            $report += @"
[$(Split-Path $issue.File -Leaf):$($issue.Line)] $($issue.Description)
  Category: $($issue.Category) | Pattern: $($issue.Pattern)
  Context:
$($issue.Context -replace "^", "    ")

"@
        }
    }

    if ($mediumIssues) {
        $report += "`n=== MEDIUM SEVERITY ===`n"
        foreach ($issue in $mediumIssues) {
            $report += @"
[$(Split-Path $issue.File -Leaf):$($issue.Line)] $($issue.Description)
  Category: $($issue.Category) | Pattern: $($issue.Pattern)

"@
        }
    }

    if ($lowIssues) {
        $report += "`n=== LOW SEVERITY ===`n"
        foreach ($issue in $lowIssues) {
            $report += "[$(Split-Path $issue.File -Leaf):$($issue.Line)] $($issue.Description)`n"
        }
    }

    if ($reviewItems) {
        $report += "`n=== CODE REVIEW ITEMS ===`n"
        foreach ($item in $reviewItems) {
            $report += "[$(Split-Path $item.File -Leaf):$($item.Line)] $($item.Description)`n"
        }
    }

    if ($Stats.IssuesFound -eq 0) {
        $report += "`n✓ No potential memory leak patterns detected!`n"
    }

    $report += @"

RECOMMENDATIONS
---------------
1. Review all HIGH severity items immediately
2. Add matching cleanup calls for every allocation
3. Use RAII patterns (unique_ptr, WDF objects) where possible
4. Enable Driver Verifier with pool tracking for testing
5. Run stress tests with Application Verifier (user-mode)

FILES WITH MOST ISSUES
----------------------
"@

    $topFiles = $Results | Group-Object File | 
                Sort-Object Count -Descending | 
                Select-Object -First 10
    
    foreach ($file in $topFiles) {
        $report += "  $(Split-Path $file.Name -Leaf): $($file.Count) issues`n"
    }

    $report += "
================================================================================
"

    return $report
}

# Main execution
Write-Log "Starting Memory Leak Audit..." "INFO"
Write-Log "Source: $SourcePath" "INFO"
Write-Log "Output: $OutputFile" "INFO"

# Clear output file
"" | Out-File $OutputFile

# Find source files
$extensions = @("*.c", "*.cpp", "*.h", "*.hpp", "*.cs")
$files = @()

foreach ($ext in $extensions) {
    $files += Get-ChildItem -Path $SourcePath -Recurse -Filter $ext -ErrorAction SilentlyContinue
}

Write-Log "Found $($files.Count) source files to scan" "INFO"

# Scan each file
$processed = 0
foreach ($file in $files) {
    Scan-File -FilePath $file.FullName
    $processed++
    
    if ($processed % 10 -eq 0) {
        Write-Log "Progress: $processed/$($files.Count) files..." "INFO"
    }
}

# Generate report
$report = Generate-Report
Write-Log "`n$report" "INFO"

# Summary
Write-Log "`n========================================" "INFO"
Write-Log "AUDIT COMPLETE" "SUCCESS"
Write-Log "Files Scanned: $($Stats.FilesScanned)" "INFO"
Write-Log "Total Issues: $($Stats.IssuesFound)" $(if ($Stats.HighSeverity -gt 0) { "WARN" } else { "SUCCESS" })
Write-Log "High Severity: $($Stats.HighSeverity)" $(if ($Stats.HighSeverity -gt 0) { "WARN" } else { "SUCCESS" })
Write-Log "Report saved to: $OutputFile" "INFO"
Write-Log "========================================" "INFO"

# Exit code based on findings
exit $(if ($Stats.HighSeverity -gt 0) { 1 } else { 0 })
