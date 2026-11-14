# Example: .\Measure-LiteMusic.ps1 -Scenario 'На паузе' -Seconds 30
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Scenario,

    [ValidateRange(5, 300)]
    [int]$Seconds = 30,

    [string]$Prototype = 'LiteMusic',

    [string]$Official = 'Яндекс Музыка'
)

$ErrorActionPreference = 'Stop'
$names = @($Prototype, $Official)
$logicalCpus = [Environment]::ProcessorCount

function Read-Processes {
    param([string[]]$Names)
    $groups = @{}
    foreach ($name in $Names) {
        $processes = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
        if ($processes.Count -eq 0) { throw "No visible processes for '$name'." }
        $cpu = @{}
        $privateBytes = [long]0
        foreach ($process in $processes) {
            if ($null -eq $process.CPU -or $process.PrivateMemorySize64 -le 0) {
                throw "Cannot read counters for '$name' (PID $($process.Id))."
            }
            $cpu.Add($process.Id, [double]$process.CPU)
            $privateBytes += $process.PrivateMemorySize64
        }
        $groups.Add($name, @{ Cpu = $cpu; Private = $privateBytes })
    }
    return $groups
}

$baseline = Read-Processes $names
$samples = @{}
foreach ($name in $names) { $samples.Add($name, [Collections.Generic.List[long]]::new()) }
$watch = [Diagnostics.Stopwatch]::StartNew()
do {
    Start-Sleep -Milliseconds 1000
    $current = Read-Processes $names
    foreach ($name in $names) {
        $pids = $baseline[$name].Cpu.Keys
        $new = $current[$name].Cpu
        if ($new.Count -ne $pids.Count -or @($pids | Where-Object { -not $new.ContainsKey($_) }).Count) {
            throw "The process group '$name' changed. Wait for it to stabilize and retry."
        }
        $samples[$name].Add($current[$name].Private)
    }
} while ($watch.Elapsed.TotalSeconds -lt $Seconds)
$watch.Stop()
$elapsed = $watch.Elapsed.TotalSeconds

foreach ($name in $names) {
    $values = @($samples[$name] | Sort-Object)
    $low = [int][Math]::Floor(($values.Count - 1) / 2)
    $high = [int][Math]::Floor($values.Count / 2)
    $cpuSeconds = [double]0
    foreach ($id in $baseline[$name].Cpu.Keys) { $cpuSeconds += $current[$name].Cpu[$id] - $baseline[$name].Cpu[$id] }
    [pscustomobject]@{
        Scenario = $Scenario
        Process = $name
        Processes = $baseline[$name].Cpu.Count
        PrivateMiB = [Math]::Round(($values[$low] / 1MB + $values[$high] / 1MB) / 2, 2)
        CPUPercent = [Math]::Round(100 * $cpuSeconds / ($elapsed * $logicalCpus), 3)
        Seconds = [Math]::Round($elapsed, 2)
        LogicalCPUs = $logicalCpus
    }
}
