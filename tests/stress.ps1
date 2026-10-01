param([string]$Root='E:\stress',[int]$Threads=8,[int]$Iters=2000)
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
public static class Stress {
 public static string Run(string root,int threads,int iters) {
  if(Directory.Exists(root)) Directory.Delete(root,true); // leftovers of an interrupted run would collide
  Directory.CreateDirectory(root);
  var errors=new System.Collections.Concurrent.ConcurrentBag<string>();
  long ops=0;
  Parallel.For(0,threads,new ParallelOptions{MaxDegreeOfParallelism=threads},t=> {
   var rng=new Random(t);
   for(int i=0;i<iters;i++) {
    string p=Path.Combine(root,"t"+t+"_"+i);
    string step="create";
    try {
     using(var f=new FileStream(p,FileMode.CreateNew,FileAccess.ReadWrite,FileShare.None)) { step="write"; f.WriteByte((byte)i); }
     Interlocked.Increment(ref ops);
     if(i%4==0) { step="mkdir"; Directory.CreateDirectory(p+".d"); step="rmdir"; Directory.Delete(p+".d"); Interlocked.Add(ref ops,2); }
     if(i%3==0) { step="rename"; File.Move(p,p+".r"); p+=".r"; Interlocked.Increment(ref ops); }
     step="delete"; File.Delete(p); Interlocked.Increment(ref ops);
     if(File.Exists(p)) errors.Add("t"+t+" i"+i+" still exists after delete: "+p);
    } catch(Exception e) { errors.Add("t"+t+" i"+i+" step="+step+" "+e.GetType().Name+": "+e.Message+" HResult=0x"+e.HResult.ToString("X8")); }
   }
  });
  var left=Directory.GetFileSystemEntries(root);
  return "ops="+ops+" errors="+errors.Count+" leftover="+left.Length+(errors.Count>0 ? Environment.NewLine+string.Join(Environment.NewLine,System.Linq.Enumerable.Take(errors,20)) : "");
 }
}
"@
$sw=[Diagnostics.Stopwatch]::StartNew()
[Stress]::Run($Root,$Threads,$Iters)
"elapsed $($sw.ElapsedMilliseconds) ms"
if((Get-ChildItem $Root -Force | Measure-Object).Count -eq 0){ [IO.Directory]::Delete($Root) }
