param([string]$Drive='E:',[int]$Runs=10,[int]$Files=200,[switch]$Keep)
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Threading.Tasks;
using System.Runtime.InteropServices;
public static class Repro {
 public static string Describe(Exception e) {
  var sb=new System.Text.StringBuilder();
  while(e!=null) {
   sb.AppendFormat("{0}: {1} HResult=0x{2:X8}", e.GetType().FullName, e.Message, e.HResult);
   var w=e as System.ComponentModel.Win32Exception; if(w!=null) sb.AppendFormat(" NativeErrorCode={0}", w.NativeErrorCode);
   sb.AppendLine();
   var ag=e as AggregateException;
   if(ag!=null) { foreach(var i in ag.InnerExceptions) sb.Append("  inner: ").Append(Describe(i)); break; }
   e=e.InnerException; if(e!=null) sb.Append("  -> ");
  }
  return sb.ToString();
 }
 public static void Write(string root,int files) {
  Parallel.For(0,4,worker=> {
   var dir=Path.Combine(root,"worker"+worker); Directory.CreateDirectory(dir); var rng=new Random(170+worker);
   string step="init"; string p="";
   try {
   for(int i=0;i<files;i++) {
    byte[] data=new byte[1024+(i*977)%65536]; rng.NextBytes(data); p=Path.Combine(dir,"f"+i+".bin");
    step="create";
    using(var f=new FileStream(p,FileMode.CreateNew,FileAccess.ReadWrite,FileShare.ReadWrite)) {
     step="write"; f.Write(data,0,data.Length);
     if(i%3==0) { step="setlength+"; f.SetLength(data.Length+8192); f.Position=data.Length+4096; step="write-tail"; f.Write(data,0,512); }
     if(i%7==0) { step="setlength-"; f.SetLength(512); }
     if(i%10==0) { step="flush"; f.Flush(true); }
     step="close";
    }
    var expected=new byte[i%7==0 ? 512 : data.Length+(i%3==0 ? 8192 : 0)]; Array.Copy(data,expected,Math.Min(data.Length,expected.Length)); if(i%3==0 && i%7!=0) Array.Copy(data,0,expected,data.Length+4096,512);
    step="readall"; var actual=File.ReadAllBytes(p); if(actual.Length!=expected.Length) throw new Exception("Length mismatch "+actual.Length+" vs "+expected.Length); for(int k=0;k<actual.Length;k++) if(actual[k]!=expected[k]) throw new Exception("Data mismatch at "+k);
    step="move"; File.Move(p,p+".renamed");
    if(i%11==0) { step="delete"; File.Delete(p+".renamed"); }
   }
   } catch(Exception e) { throw new Exception("worker"+worker+" step="+step+" path="+p+" :: "+Describe(e), e); }
  });
 }
}
"@
$fail=0
for($r=1;$r -le $Runs;$r++){
 $root="$Drive\rep$r"
 if(Test-Path -LiteralPath $root){ [IO.Directory]::Delete($root,$true) }
 [IO.Directory]::CreateDirectory($root) | Out-Null
 $sw=[Diagnostics.Stopwatch]::StartNew()
 try { [Repro]::Write($root,$Files); "run $r OK $($sw.ElapsedMilliseconds) ms" }
 catch { $fail++; "run $r FAIL $($sw.ElapsedMilliseconds) ms"; [Repro]::Describe($_.Exception); if($Keep){ continue } }
 if(-not $Keep){ [IO.Directory]::Delete($root,$true) }
}
"done: $Runs runs, $fail failed"
