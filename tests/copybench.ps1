# copybench.ps1 - write, copy and read MB megabytes on each target drive (guest)
#   powershell -File copybench.ps1 [-MB 256] [-Targets C,E]
param([int]$MB=256, [string]$Targets='C,E')
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Runtime.InteropServices;
public static class CB {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateFile(string p,uint a,uint s,IntPtr sa,uint d,uint f,IntPtr t);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool FlushFileBuffers(IntPtr h);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 public static void FlushVolume(string v){ var h=CreateFile(v,0xC0000000,7,IntPtr.Zero,3,0,IntPtr.Zero); if(h==new IntPtr(-1)) throw new System.ComponentModel.Win32Exception(); try{ if(!FlushFileBuffers(h)) throw new System.ComponentModel.Win32Exception(); } finally { CloseHandle(h); } }
 // SetLength first (like CopyFile does), then stream the data
 public static long WriteExt(string p,long size){ var buf=new byte[1<<20]; new Random(1).NextBytes(buf); var sw=System.Diagnostics.Stopwatch.StartNew(); using(var f=new FileStream(p,FileMode.Create,FileAccess.Write,FileShare.None,1<<20)){ f.SetLength(size); for(long o=0;o<size;o+=buf.Length) f.Write(buf,0,buf.Length); f.Flush(true);} return sw.ElapsedMilliseconds; }
 // plain streaming write, no preallocation
 public static long WritePlain(string p,long size){ var buf=new byte[1<<20]; new Random(1).NextBytes(buf); var sw=System.Diagnostics.Stopwatch.StartNew(); using(var f=new FileStream(p,FileMode.Create,FileAccess.Write,FileShare.None,1<<20)){ for(long o=0;o<size;o+=buf.Length) f.Write(buf,0,buf.Length); f.Flush(true);} return sw.ElapsedMilliseconds; }
 public static long Read(string p){ var buf=new byte[1<<20]; var sw=System.Diagnostics.Stopwatch.StartNew(); using(var f=new FileStream(p,FileMode.Open,FileAccess.Read,FileShare.Read,1<<20,FileOptions.SequentialScan)){ while(f.Read(buf,0,buf.Length)>0){} } return sw.ElapsedMilliseconds; }
}
"@
$size=[long]$MB*1MB
$src="C:\Users\Administrator\src$MB.bin"
if(-not (Test-Path $src)){ [CB]::WritePlain($src,$size) | Out-Null }
foreach($t in @($Targets -split '[,\s]+' | Where-Object { $_ } | ForEach-Object { if ($_ -eq 'C') { 'C:\Users\Administrator\bench-dst.bin' } else { "$($_):\bench-dst.bin" } })){
 if(Test-Path $t){ [IO.File]::Delete($t) }
 $vol='\\.\'+$t.Substring(0,2)
 $w1=[CB]::WriteExt($t,$size); [CB]::FlushVolume($vol); [IO.File]::Delete($t)
 $w2=[CB]::WritePlain($t,$size); [CB]::FlushVolume($vol); [IO.File]::Delete($t)
 $sw=[Diagnostics.Stopwatch]::StartNew(); [IO.File]::Copy($src,$t,$true); [CB]::FlushVolume($vol); $c=$sw.ElapsedMilliseconds
 $r=[CB]::Read($t); [IO.File]::Delete($t)
 "{0}  setlength+write {1} ms   plain write {2} ms   File.Copy {3} ms   read(cached) {4} ms" -f $t.Substring(0,2),$w1,$w2,$c,$r
}
[IO.File]::Delete($src)   # nothing left behind on the guest
