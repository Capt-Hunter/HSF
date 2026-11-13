param([int]$ProcessId, [string]$Path)
# Writes a full-memory minidump of a (hung) process, for tools in HSF\tests to read. Test processes only (see guard.ps1).
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Dbg {
  [DllImport("dbghelp.dll", SetLastError = true)]
  public static extern bool MiniDumpWriteDump(IntPtr process, uint processId, SafeHandle file, uint type, IntPtr e, IntPtr u, IntPtr c);
}
"@
$p = Get-Process -Id $ProcessId
$f = [System.IO.File]::Create($Path)
$ok = [Dbg]::MiniDumpWriteDump($p.Handle, [uint32]$ProcessId, $f.SafeFileHandle, 2, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
$f.Close()
"dump written: $ok ($Path)"
