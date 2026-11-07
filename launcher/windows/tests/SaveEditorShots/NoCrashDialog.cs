using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

/// <summary>A failed check or any other unhandled exception ends the run with exit code 1 and the message on stderr, never
/// with a Windows error dialog (the tests run on a PC where someone may be playing). Runs before Main.</summary>
internal static class NoCrashDialog
{
    private const uint SemFailCriticalErrors = 0x0001, SemNoGpFaultErrorBox = 0x0002, SemNoOpenFileErrorBox = 0x8000;

    [DllImport("kernel32.dll")]
    private static extern uint SetErrorMode(uint mode);

    [ModuleInitializer]
    internal static void Install()
    {
        SetErrorMode(SemFailCriticalErrors | SemNoGpFaultErrorBox | SemNoOpenFileErrorBox);
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            Console.Out.Flush();
            Console.Error.WriteLine("FAIL (unhandled): " + e.ExceptionObject);
            Console.Error.Flush();
            Environment.Exit(1);
        };
    }
}
