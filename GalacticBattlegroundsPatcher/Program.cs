using KPatchCore.Applicators;
using KPatchCore.Launcher;
using KPatchCore.Managers;
using System.ComponentModel;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Security.Principal;

namespace GalacticBattlegroundsPatcher;

internal static class Program
{
    private const string PreferredGameExecutableName = "battlegrounds_x2.exe";
    private const string FallbackGameExecutableName = "battlegrounds_x1.exe";
    private const string RuntimeDllName = "GalacticBattlegroundsPatcher.Runtime.dll";

    private static int Main(string[] args)
    {
        if (args.Length > 2 || args.Any(arg => arg is "-h" or "--help"))
        {
            PrintUsage();
            return args.Length > 2 ? 1 : 0;
        }

        string? extractedRuntimePath = null;
        try
        {
            var baseDirectory = AppContext.BaseDirectory;
            var gamePath = Path.GetFullPath(
                args.Length >= 1 ? args[0] : FindDefaultGameExecutable(baseDirectory));
            var patchesDirectory = Path.GetFullPath(
                args.Length >= 2 ? args[1] : Path.Combine(baseDirectory, "patches"));
            extractedRuntimePath = ExtractRuntimeDll();

            if (!File.Exists(gamePath))
            {
                return Fail($"Game executable not found: {gamePath}");
            }

            if (Path.GetFileName(gamePath).Equals(
                    FallbackGameExecutableName,
                    StringComparison.OrdinalIgnoreCase) &&
                !IsElevated())
            {
                return RelaunchElevated(args, baseDirectory);
            }

            if (!Directory.Exists(patchesDirectory))
            {
                return Fail($"Patches directory not found: {patchesDirectory}");
            }

            Console.WriteLine($"Game:    {gamePath}");
            Console.WriteLine($"Patches: {patchesDirectory}");

            var orchestrator = new PatchOrchestrator(patchesDirectory);
            var scanResult = orchestrator.RescanPatches();
            if (!scanResult.Success)
            {
                return Fail(scanResult.Error ?? "Patch scan failed.");
            }

            string gameHash;
            using (var gameStream = File.OpenRead(gamePath))
            {
                gameHash = Convert.ToHexString(SHA256.HashData(gameStream));
            }
            var patchIds = orchestrator.GetAvailablePatches()
                .Where(entry => entry.Value.Manifest.SupportedVersions.Values.Any(
                    hash => hash.Equals(gameHash, StringComparison.OrdinalIgnoreCase)))
                .Select(entry => entry.Key)
                .OrderBy(id => id, StringComparer.OrdinalIgnoreCase)
                .ToList();
            if (patchIds.Count == 0)
            {
                return Fail($"No patches support {Path.GetFileName(gamePath)} ({gameHash}).");
            }

            var status = PatchRemover.HasPatchesInstalled(gamePath);
            if (!status.Success)
            {
                return Fail(status.Error ?? "Could not inspect the current patch state.");
            }

            if (status.Data)
            {
                Console.WriteLine("Restoring the previous managed patch state...");
                var removal = PatchRemover.RemoveAllPatches(gamePath, removeManagedState: false);
                if (!removal.Success)
                {
                    return Fail(removal.Error ?? "Could not remove the previous patch state.");
                }
            }

            Console.WriteLine($"Applying {patchIds.Count} patch(es): {string.Join(", ", patchIds)}");
            var install = orchestrator.InstallPatches(
                gamePath,
                patchIds,
                createBackup: true,
                patcherDllPath: extractedRuntimePath);
            if (!install.Success)
            {
                return Fail(install.Error ?? "Patch installation failed.");
            }

            Console.WriteLine($"Launching {Path.GetFileName(gamePath)}...");
            var launch = GameLauncher.LaunchPatchedGame(gamePath);
            if (!launch.Success)
            {
                return Fail(launch.Error ?? "Game launch failed.");
            }

            Console.WriteLine(
                launch.GameProcess == null
                    ? "Galactic Battlegrounds launched with all patches."
                    : $"Galactic Battlegrounds launched with all patches (PID {launch.GameProcess.Id}).");
            return 0;
        }
        catch (Exception ex)
        {
            return Fail(ex.Message);
        }
        finally
        {
            if (extractedRuntimePath != null)
            {
                try
                {
                    File.Delete(extractedRuntimePath);
                }
                catch
                {
                    // The installed copy in the game directory is used after this point.
                }
            }
        }
    }

    private static int Fail(string message)
    {
        Console.Error.WriteLine($"ERROR: {message}");
        return 1;
    }

    private static string FindDefaultGameExecutable(string baseDirectory)
    {
        var preferred = Path.Combine(baseDirectory, PreferredGameExecutableName);
        if (File.Exists(preferred)) return preferred;

        var fallback = Path.Combine(baseDirectory, FallbackGameExecutableName);
        return File.Exists(fallback) ? fallback : preferred;
    }

    private static bool IsElevated()
    {
        if (!OperatingSystem.IsWindows()) return false;
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static int RelaunchElevated(string[] args, string workingDirectory)
    {
        var launcherPath = Environment.ProcessPath
            ?? throw new InvalidOperationException("Could not determine the launcher executable path.");
        var startInfo = new ProcessStartInfo
        {
            FileName = launcherPath,
            UseShellExecute = true,
            Verb = "runas",
            WorkingDirectory = workingDirectory
        };
        foreach (var arg in args) startInfo.ArgumentList.Add(arg);

        try
        {
            Console.WriteLine("battlegrounds_x1.exe requires administrator privileges; requesting elevation...");
            Process.Start(startInfo);
            return 0;
        }
        catch (Win32Exception ex) when (ex.NativeErrorCode == 1223)
        {
            return Fail("Administrator elevation was canceled.");
        }
    }

    private static string ExtractRuntimeDll()
    {
        using var source = typeof(Program).Assembly.GetManifestResourceStream(RuntimeDllName)
            ?? throw new InvalidOperationException("Embedded patch runtime was not found.");

        var path = Path.Combine(
            Path.GetTempPath(),
            $"GalacticBattlegroundsPatcher.Runtime.{Environment.ProcessId}.dll");
        using var destination = File.Create(path);
        source.CopyTo(destination);
        return path;
    }

    private static void PrintUsage()
    {
        Console.WriteLine("GalacticBattlegroundsLauncher [game-executable] [patches-directory]");
        Console.WriteLine();
        Console.WriteLine("Defaults:");
        Console.WriteLine(
            $"  game-executable:   .\\{PreferredGameExecutableName}, then .\\{FallbackGameExecutableName}");
        Console.WriteLine("  patches-directory: .\\patches");
    }
}
