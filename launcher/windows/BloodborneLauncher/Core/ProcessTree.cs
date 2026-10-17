using System.Diagnostics;
using System.Runtime.InteropServices;

namespace BloodborneLauncher.Core;

internal static class ProcessTree
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Entry
    {
        public uint size, usage, id;
        public UIntPtr heap;
        public uint module, threads, parent;
        public int priority;
        public uint flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string name;
    }
    [DllImport("kernel32.dll")] private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint id);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool Process32FirstW(IntPtr snapshot, ref Entry entry);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool Process32NextW(IntPtr snapshot, ref Entry entry);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);

    public static IEnumerable<Process> Descendants(int parent)
    {
        var entries = new List<(int Id, int Parent)>();
        var snapshot = CreateToolhelp32Snapshot(2, 0);
        if (snapshot == new IntPtr(-1)) return Array.Empty<Process>();
        try
        {
            var entry = new Entry { size = (uint)Marshal.SizeOf<Entry>(), name = "" };
            if (Process32FirstW(snapshot, ref entry))
                do { entries.Add(((int)entry.id, (int)entry.parent)); } while (Process32NextW(snapshot, ref entry));
        }
        finally { CloseHandle(snapshot); }
        var ids = new HashSet<int> { parent };
        while (true)
        {
            var next = entries.Where(e => ids.Contains(e.Parent) && !ids.Contains(e.Id)).Select(e => e.Id).ToArray();
            if (next.Length == 0) break;
            ids.UnionWith(next);
        }
        var result = new List<Process>();
        foreach (var id in ids.Where(id => id != parent))
            try { result.Add(Process.GetProcessById(id)); } catch (ArgumentException) { }
        return result;
    }

    public static Process[] GamesIn(string engine)
    {
        var expected = System.IO.Path.GetFullPath(System.IO.Path.Combine(engine, "out", "bb-probe.exe"));
        var result = new List<Process>();
        foreach (var process in Process.GetProcessesByName("bb-probe"))
        {
            try
            {
                if (string.Equals(process.MainModule?.FileName, expected, StringComparison.OrdinalIgnoreCase))
                { result.Add(process); continue; }
            }
            catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { }
            process.Dispose();
        }
        return result.ToArray();
    }
}
