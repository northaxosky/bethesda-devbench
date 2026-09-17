namespace BethesdaDevBench.WindowsSession
{
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

    internal static class ShimNative
    {
        [DllImport("shell32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern IntPtr CommandLineToArgvW(string command, out int count);
        [DllImport("kernel32.dll")]
        private static extern IntPtr LocalFree(IntPtr memory);

        internal static string[] Arguments(string command)
        {
            int count;
            IntPtr memory = CommandLineToArgvW("devbench-launch.exe " + command, out count);
            if (memory == IntPtr.Zero)
                throw new BoundaryException("INVALID_SHIM_CONFIG", "Cannot parse the MO2 launch arguments.");
            try
            {
                string[] result = new string[count - 1];
                for (int index = 1; index < count; index++)
                    result[index - 1] = Marshal.PtrToStringUni(Marshal.ReadIntPtr(memory, index * IntPtr.Size));
                return result;
            }
            finally { LocalFree(memory); }
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool GetNamedPipeClientProcessId(IntPtr pipe, out uint pid);
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint pid);
        [DllImport("kernel32.dll")]
        internal static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint GetProcessId(SafeProcessHandle process);
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool DuplicateHandle(
            IntPtr sourceProcess, IntPtr source, IntPtr targetProcess,
            out IntPtr target, uint access, bool inherit, uint options);

        internal static SafeProcessHandle Duplicate(IntPtr owner, IntPtr source)
        {
            IntPtr handle;
            if (!DuplicateHandle(owner, source, GetCurrentProcess(), out handle, 0, false, 2))
                throw new BoundaryException("HANDLE_TRANSFER_FAILED",
                    "DuplicateHandle failed: " + Marshal.GetLastWin32Error());
            return new SafeProcessHandle(handle);
        }
    }

    internal sealed class PipeWire : IDisposable
    {
        private readonly PipeStream pipe_;
        private readonly CancellationTokenSource deadline_;
        internal PipeWire(PipeStream pipe, int timeoutMs)
        {
            pipe_ = pipe;
            deadline_ = new CancellationTokenSource(timeoutMs);
        }

        internal void Write(string value)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(value);
            if (bytes.Length > 65536)
                throw new BoundaryException("SHIM_PROTOCOL_ERROR", "Shim field exceeds its bound.");
            byte[] size = BitConverter.GetBytes(bytes.Length);
            pipe_.WriteAsync(size, 0, size.Length, deadline_.Token).GetAwaiter().GetResult();
            pipe_.WriteAsync(bytes, 0, bytes.Length, deadline_.Token).GetAwaiter().GetResult();
            pipe_.FlushAsync(deadline_.Token).GetAwaiter().GetResult();
        }

        private byte[] ReadBytes(int count)
        {
            byte[] bytes = new byte[count];
            int offset = 0;
            while (offset < count)
            {
                int read = pipe_.ReadAsync(bytes, offset, count - offset, deadline_.Token)
                    .GetAwaiter().GetResult();
                if (read == 0)
                    throw new BoundaryException("SHIM_DISCONNECTED", "The launch peer disconnected.");
                offset += read;
            }
            return bytes;
        }

        internal string Read()
        {
            int size = BitConverter.ToInt32(ReadBytes(4), 0);
            if (size < 0 || size > 65536)
                throw new BoundaryException("SHIM_PROTOCOL_ERROR", "Invalid launch field length.");
            return new UTF8Encoding(false, true).GetString(ReadBytes(size));
        }

        public void Dispose() { deadline_.Dispose(); }
    }

    public static class LaunchShim
    {
        private const string Mo2Pipe = "mo-43d1a3ad-eeb0-4818-97c9-eda5216c29b5";

        private static string PipeName(string game)
        {
            using (SHA256 hash = SHA256.Create())
                return "BethesdaDevBench.Launch." +
                    BitConverter.ToString(hash.ComputeHash(Encoding.UTF8.GetBytes(game.ToUpperInvariant())))
                        .Replace("-", "");
        }

        private static bool Same(string left, string right)
        {
            return String.Equals(left, right, StringComparison.OrdinalIgnoreCase);
        }

        private static int VerifyMo2Ancestor(int shimPid, string mo2Image, long requestStarted)
        {
            Dictionary<int, int> parents = WindowsProcess.SnapshotParents();
            ProcessIdentity child = WindowsProcess.TryGetIdentity(shimPid);
            if (child == null || child.CreationFileTime < requestStarted)
                throw new BoundaryException("SHIM_IDENTITY_MISMATCH", "Shim predates this launch request.");
            for (int depth = 0; depth < 16; depth++)
            {
                int parentPid;
                if (!parents.TryGetValue(child.Pid, out parentPid) || parentPid <= 0)
                    break;
                ProcessIdentity parent = WindowsProcess.TryGetIdentity(parentPid);
                if (parent == null || parent.CreationFileTime > child.CreationFileTime)
                    break;
                if (Same(parent.ImagePath, mo2Image))
                    return parent.Pid;
                child = parent;
            }
            throw new BoundaryException("OWNERSHIP_UNPROVEN",
                "The connected launch shim has no live ancestry to the configured MO2.");
        }

        internal static OwnedSession Start(
            string mo2, string profile, string label, string game, string loader, int timeoutMs)
        {
            long requested = DateTime.UtcNow.ToFileTimeUtc();
            string nonce = Guid.NewGuid().ToString("N");
            using (NamedPipeServerStream pipe = new NamedPipeServerStream(
                PipeName(game), PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly))
            using (CancellationTokenSource deadline = new CancellationTokenSource(timeoutMs))
            using (PipeWire wire = new PipeWire(pipe, timeoutMs))
            {
                var connected = pipe.WaitForConnectionAsync(deadline.Token);
                using (NamedPipeClientStream forwarding = new NamedPipeClientStream(
                    ".", Mo2Pipe, PipeDirection.Out,
                    PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly))
                {
                    forwarding.ConnectAsync(timeoutMs, deadline.Token).GetAwaiter().GetResult();
                    uint primaryPid;
                    if (!ShimNative.GetNamedPipeServerProcessId(
                        forwarding.SafePipeHandle.DangerousGetHandle(), out primaryPid))
                        throw new BoundaryException("MO2_IDENTITY_MISMATCH", "Cannot identify MO2's forwarding server.");
                    ProcessIdentity primary = WindowsProcess.TryGetIdentity((int)primaryPid);
                    if (primary == null || !Same(primary.ImagePath, mo2))
                        throw new BoundaryException("MO2_IDENTITY_MISMATCH",
                            "The single-instance pipe belongs to another MO2 installation.");
                    // Match MO2's CommandLine::forwardToPrimary payload; keep it open until the shim replies.
                    string command = Controller.QuoteWindowsArgument(mo2) + " -p " +
                        Controller.QuoteWindowsArgument(profile) + " run -e " +
                        Controller.QuoteWindowsArgument(label);
                    byte[] bytes = Encoding.UTF8.GetBytes(command);
                    forwarding.WriteAsync(bytes, 0, bytes.Length, deadline.Token).GetAwaiter().GetResult();
                    forwarding.FlushAsync(deadline.Token).GetAwaiter().GetResult();
                    connected.GetAwaiter().GetResult();
                    uint peer;
                    if (!ShimNative.GetNamedPipeClientProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out peer))
                        throw new BoundaryException("SHIM_IDENTITY_MISMATCH", "Cannot identify the pipe client.");
                    ProcessIdentity shim = WindowsProcess.TryGetIdentity((int)peer);
                    ProcessIdentity self = WindowsProcess.TryGetIdentity(Process.GetCurrentProcess().Id);
                    string expectedShim = WindowsProcess.CanonicalizeExistingPath(
                        Path.Combine(Path.GetDirectoryName(Controller.ShimScriptPath), "devbench-launch.exe"), false);
                    if (shim == null || !Same(shim.ImagePath, expectedShim))
                        throw new BoundaryException("SHIM_IDENTITY_MISMATCH", "Launch peer is not the packaged native shim.");
                    int mo2Pid = VerifyMo2Ancestor((int)peer, mo2, requested);
                    using (SafeProcessHandle shimHandle = WindowsProcess.OpenProcessHandle(
                        (int)peer, 0x0040 | NativeMethods.PROCESS_QUERY_LIMITED_INFORMATION |
                            NativeMethods.SYNCHRONIZE, "SHIM_PROCESS_OPEN_FAILED"))
                    {
                        if (WindowsProcess.QueryCreationFileTime(shimHandle) != shim.CreationFileTime)
                            throw new BoundaryException("SHIM_IDENTITY_MISMATCH", "Shim PID changed.");
                        wire.Write("devbench-launch/1");
                        wire.Write(nonce);
                        wire.Write(self.Pid.ToString(CultureInfo.InvariantCulture));
                        wire.Write(self.CreationFileTime.ToString(CultureInfo.InvariantCulture));
                        wire.Write(game);
                        wire.Write(loader);
                        wire.Write((requested + TimeSpan.FromMilliseconds(timeoutMs).Ticks)
                            .ToString(CultureInfo.InvariantCulture));
                        if (wire.Read() != nonce)
                            throw new BoundaryException("SHIM_PROTOCOL_ERROR", "Launch challenge did not match.");
                        wire.Write("launch");
                        string outcome = wire.Read();
                        if (outcome != "owned")
                            throw new BoundaryException("SHIM_START_FAILED", wire.Read());
                        if (wire.Read() != nonce)
                            throw new BoundaryException("SHIM_PROTOCOL_ERROR", "Ownership challenge did not match.");
                        int pid = Int32.Parse(wire.Read(), CultureInfo.InvariantCulture);
                        long creation = Int64.Parse(wire.Read(), CultureInfo.InvariantCulture);
                        long source = Int64.Parse(wire.Read(), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                        SafeProcessHandle handle = ShimNative.Duplicate(
                            shimHandle.DangerousGetHandle(), new IntPtr(source));
                        try
                        {
                            if (ShimNative.GetProcessId(handle) != pid ||
                                WindowsProcess.QueryCreationFileTime(handle) != creation ||
                                !Same(WindowsProcess.QueryImagePath(handle), game) ||
                                !WindowsProcess.IsRunning(handle))
                                throw new BoundaryException("PROCESS_IDENTITY_CHANGED", "Transferred game handle identity differs.");
                            wire.Write("retained");
                            OwnedSession session = new OwnedSession {
                                Handle = handle, Pid = pid, CreationFileTime = creation,
                                CreationTime = WindowsProcess.ToIso(creation), ImagePath = game,
                                Mo2Pid = mo2Pid, Running = true,
                                ShimHandle = ShimNative.Duplicate(
                                    ShimNative.GetCurrentProcess(), shimHandle.DangerousGetHandle())
                            };
                            handle = null;
                            return session;
                        }
                        finally { if (handle != null) handle.Dispose(); }
                    }
                }
            }
        }

    }
}
