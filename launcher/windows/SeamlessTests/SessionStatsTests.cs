using BloodborneLauncher.Core;

/// <summary>SessionStats: stutters count during play only; stalls while loading are counted apart.</summary>
static class SessionStatsTests
{
    private static void Check(bool ok, string what)
    {
        if (!ok) throw new InvalidOperationException("SessionStats: " + what);
    }

    private const string Stall = "Stall: 119.1 ms frame (loading or other work); test";
    private const string Begins = "Loading: begins; pacer mode 3, test";
    private const string Ends = "Loading: 3.17 s, 95 frames (33.4 ms each); pacer mode 3";
    private const string First = "Loading: the first in-game frame, 23.4 s after the game started";

    private static string Frame(string fps) => $"Frame stats: {fps} FPS, worst frame 17.1 ms, test";

    private static SessionStats Feed(params string[] lines)
    {
        var s = new SessionStats();
        foreach (var l in lines) s.Observe(l);
        return s;
    }

    public static void Run()
    {
        var a = Feed(Stall, First);
        Check(a.Stutters == 0 && a.LoadingStutters == 1, "a start-up stall is a loading stutter");

        var b = Feed(First, Frame("59.9"), Frame("59.9"), Stall);
        Check(b.Stutters == 1 && b.LoadingStutters == 0, "a stall after a clean window counts");

        var c = Feed(First, Frame("59.9"), Frame("59.9"), Begins, Stall, Ends, Stall, Frame("59.9"), Frame("59.9"), Stall);
        Check(c.LoadingStutters == 2 && c.Stutters == 1, "load and quiet window stalls are apart: " + c.LoadingStutters + "/" + c.Stutters);

        var d = Feed(First, Frame("59.9"), Begins, Ends, Frame("30.0"), Frame("59.9"), Frame("59.9"));
        Check(d.HasFrameStats && d.LowestFps > 59.8 && d.LowestFps < 60, "a window with a load is not in the lowest FPS: " + d.LowestFps);
        var d2 = Feed(First, Begins, Ends, Frame("30.0"));
        Check(d2.HasFrameStats && d2.LowestFps == 0 && d2.AverageFps == 0, "no clean window: no FPS, frame stats still seen");

        var e = new SessionStats();
        for (int i = 0; i < 12; i++) e.Observe(Frame("59.9"));
        e.Observe(Stall);
        Check(e.Stutters == 1 && e.LoadingStutters == 0, "an old log without Loading lines counts as before");

        var f = Feed(First, Frame("59.9"), Frame("59.9"), Begins, Stall, Ends, Stall, Frame("59.9"), Frame("59.9"), Stall);
        Check(f.Summary.Contains("(+2 while loading)"), "summary: " + f.Summary);
        Console.WriteLine("PASS: session stutters count during play only, loading stalls apart");
    }
}
