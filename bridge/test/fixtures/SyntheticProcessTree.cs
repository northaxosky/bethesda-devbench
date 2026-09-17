using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Threading;

internal static class SyntheticProcessTree
{
    private static string CurrentImage()
    {
        return Assembly.GetEntryAssembly().Location;
    }

    private static void WritePid(string directory, string role)
    {
        File.WriteAllText(
            Path.Combine(directory, role + ".pid"),
            Process.GetCurrentProcess().Id.ToString());
    }

    private static int ReadDelay(string directory)
    {
        string path = Path.Combine(directory, "delay-ms.txt");
        if (!File.Exists(path))
        {
            return 0;
        }

        int delay;
        return Int32.TryParse(File.ReadAllText(path).Trim(), out delay)
            ? Math.Max(0, Math.Min(delay, 10000))
            : 0;
    }

    public static int Main(string[] args)
    {
        string image = CurrentImage();
        string directory = Path.GetDirectoryName(image);
        string name = Path.GetFileNameWithoutExtension(image);

        if (String.Equals(name, "SyntheticMo2", StringComparison.OrdinalIgnoreCase))
        {
            string forwarding = Path.Combine(directory, "forward.pipe");
            if (args.Length == 0 || args[0] != "--serve") return 3;
            WritePid(directory, "mo2");
            string pipeName = File.ReadAllText(Path.Combine(directory, "ipc-name.txt"));
            using (NamedPipeServerStream server = new NamedPipeServerStream(
                pipeName, PipeDirection.In, 1))
            {
                File.WriteAllText(forwarding, pipeName);
                server.WaitForConnection();
                Thread.Sleep(ReadDelay(directory));
                byte[] message = new byte[4096];
                int count = server.Read(message, 0, message.Length);
                File.WriteAllText(Path.Combine(directory, "forwarded-command.txt"),
                    System.Text.Encoding.UTF8.GetString(message, 0, count));
                File.Delete(forwarding);
                string[] config = File.ReadAllLines(Path.Combine(directory, "shim-config.txt"));
                using (Process shim = Process.Start(new ProcessStartInfo {
                    FileName = config[0],
                    Arguments = config[1],
                    UseShellExecute = false,
                    WorkingDirectory = directory
                }))
                {
                    File.WriteAllText(Path.Combine(directory, "shim.pid"), shim.Id.ToString());
                    shim.WaitForExit();
                }
            }
            return 0;
        }

        if (String.Equals(name, "SyntheticLoader", StringComparison.OrdinalIgnoreCase))
        {
            WritePid(directory, "loader");
            Process.Start(new ProcessStartInfo {
                FileName = Path.Combine(directory, "SyntheticGame.exe"),
                UseShellExecute = false,
                WorkingDirectory = directory
            });
            return 0;
        }

        if (String.Equals(name, "SyntheticGame", StringComparison.OrdinalIgnoreCase))
        {
            WritePid(directory, "game");
            Stopwatch lifetime = Stopwatch.StartNew();
            while (lifetime.Elapsed < TimeSpan.FromSeconds(30))
            {
                Thread.Sleep(100);
            }
            return 0;
        }

        return 2;
    }
}
