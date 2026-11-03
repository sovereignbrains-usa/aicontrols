param([string]$Path, [int]$Max = 80, [int]$HexLen = 0)
# A minidump without a debugger: the crashing thread, its registers and every word on its stack that points into a
# module (as module+offset). Enough to name the function from a map file or a PDB.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
public class Mod { public ulong b; public uint size; public string name; }
public class Reg { public ulong start; public ulong size; public long rva; }
public static class DumpScan {
    public static string Run(string path, int max, int hexLen) {
        var b = File.ReadAllBytes(path);
        Func<int, uint> u32 = o => BitConverter.ToUInt32(b, o);
        Func<long, ulong> u64 = o => BitConverter.ToUInt64(b, (int)o);
        var sb = new StringBuilder();
        int streams = (int)u32(8); int dir = (int)u32(12);
        var at = new Dictionary<uint, int[]>();
        for (int i = 0; i < streams; i++) { int o = dir + i * 12; at[u32(o)] = new[] { (int)u32(o + 4), (int)u32(o + 8) }; }
        // modules
        var mods = new List<Mod>();
        if (at.ContainsKey(4)) {
            int o = at[4][1]; int n = (int)u32(o);
            for (int i = 0; i < n; i++) {
                int m = o + 4 + i * 108;
                ulong bas = u64(m); uint size = u32(m + 8); int nr = (int)u32(m + 20);
                int len = (int)u32(nr); string name = Encoding.Unicode.GetString(b, nr + 4, len);
                mods.Add(new Mod { b = bas, size = size, name = name });
            }
        }
        Func<ulong, string> where = a => {
            foreach (var m in mods) if (a >= m.b && a < m.b + m.size) return Path.GetFileName(m.name) + "+0x" + (a - m.b).ToString("x");
            return null;
        };
        // memory
        var mem = new List<Reg>();
        if (at.ContainsKey(9)) {
            int o = at[9][1]; ulong n = u64(o); long data = (long)u64(o + 8);
            for (ulong i = 0; i < n; i++) { int d = o + 16 + (int)i * 16; ulong s = u64(d), z = u64(d + 8); mem.Add(new Reg { start = s, size = z, rva = data }); data += (long)z; }
        }
        if (at.ContainsKey(5)) {
            int o = at[5][1]; int n = (int)u32(o);
            for (int i = 0; i < n; i++) { int d = o + 4 + i * 16; mem.Add(new Reg { start = u64(d), size = u32(d + 8), rva = u32(d + 12) }); }
        }
        // exception
        int ex = at[6][1];
        uint tid = u32(ex); uint code = u32(ex + 8); ulong eaddr = u64(ex + 24);
        int ctx = (int)u32(ex + 8 + 152 + 4);
        ulong rip = u64(ctx + 0xF8), rsp = u64(ctx + 0x98), rbp = u64(ctx + 0xA0);
        sb.AppendLine("thread " + tid + " code 0x" + code.ToString("x") + " addr " + (where(eaddr) ?? eaddr.ToString("x")));
        sb.AppendLine("rip " + (where(rip) ?? rip.ToString("x")) + "  rsp " + rsp.ToString("x") + "  rbp " + rbp.ToString("x"));
        sb.AppendLine("modules:"); foreach (var m in mods) sb.AppendLine("  " + m.b.ToString("x") + " " + m.size.ToString("x") + " " + m.name);
        sb.AppendLine("stack words that point into a module:");
        int shown = 0;
        foreach (var r in mem) {
            if (rsp >= r.start && rsp < r.start + r.size) {
                for (ulong a = rsp; a + 8 <= r.start + r.size && shown < max; a += 8) {
                    ulong v = u64(r.rva + (long)(a - r.start));
                    string w = where(v);
                    if (w != null) { sb.AppendLine("  rsp+0x" + (a - rsp).ToString("x") + "  " + w); shown++; }
                }
            }
        }
        if (hexLen > 0) {
            sb.AppendLine("raw stack from rsp:");
            foreach (var r in mem) {
                if (rsp >= r.start && rsp < r.start + r.size) {
                    for (ulong a = rsp; a + 16 <= r.start + r.size && a < rsp + (ulong)hexLen; a += 16) {
                        long off = r.rva + (long)(a - r.start);
                        var h = new StringBuilder(); var c = new StringBuilder();
                        for (int k = 0; k < 16; k++) { byte x = b[off + k]; h.Append(x.ToString("x2")).Append(k == 7 ? "  " : " "); c.Append(x >= 32 && x < 127 ? (char)x : (char)46); }
                        sb.AppendLine("  +" + (a - rsp).ToString("x4") + "  " + h + " " + c);
                    }
                }
            }
        }
        return sb.ToString();
    }
}
'@
[DumpScan]::Run($Path, $Max, $HexLen)

