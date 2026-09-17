#Requires -Version 7.0
[CmdletBinding()]
param()

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$source = @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace BethesdaDevBench.WindowsSession
{
    public sealed class BoundaryException : Exception
    {
        public BoundaryException(string code, string message)
            : base(message)
        {
            Code = code;
        }

        public string Code { get; private set; }
    }

    public sealed class LineReadResult
    {
        public bool EndOfStream;
        public bool TooLong;
        public string Line;
    }

    public static class BoundedLineReader
    {
        public static LineReadResult Read(TextReader reader, int maximumCharacters)
        {
            StringBuilder builder = new StringBuilder(Math.Min(maximumCharacters, 4096));
            bool tooLong = false;

            while (true)
            {
                int value = reader.Read();
                if (value < 0)
                {
                    if (builder.Length == 0 && !tooLong)
                    {
                        return new LineReadResult { EndOfStream = true };
                    }

                    return new LineReadResult {
                        EndOfStream = false,
                        TooLong = tooLong,
                        Line = builder.ToString()
                    };
                }

                char character = (char)value;
                if (character == '\n')
                {
                    return new LineReadResult {
                        EndOfStream = false,
                        TooLong = tooLong,
                        Line = builder.ToString()
                    };
                }

                if (character == '\r')
                {
                    continue;
                }

                if (builder.Length < maximumCharacters)
                {
                    builder.Append(character);
                }
                else
                {
                    tooLong = true;
                }
            }
        }
    }

    internal sealed class SafeProcessHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        private SafeProcessHandle()
            : base(true)
        {
        }

        internal SafeProcessHandle(IntPtr handle) : base(true)
        {
            SetHandle(handle);
        }

        protected override bool ReleaseHandle()
        {
            return NativeMethods.CloseHandle(handle);
        }
    }

    internal sealed class ProcessIdentity
    {
        public int Pid;
        public int ParentPid;
        public long CreationFileTime;
        public string CreationTime;
        public string ImagePath;
    }

    internal sealed class OwnedSession : IDisposable
    {
        public SafeProcessHandle Handle;
        public SafeProcessHandle ShimHandle;
        public int Pid;
        public long CreationFileTime;
        public string CreationTime;
        public string ImagePath;
        public int Mo2Pid;
        public bool Running;

        public void Dispose()
        {
            if (Handle != null)
            {
                Handle.Dispose();
                Handle = null;
            }
            if (ShimHandle != null)
            {
                ShimHandle.Dispose();
                ShimHandle = null;
            }
        }
    }

    internal static class NativeMethods
    {
        internal const uint PROCESS_TERMINATE = 0x0001;
        internal const uint PROCESS_VM_READ = 0x0010;
        internal const uint PROCESS_QUERY_INFORMATION = 0x0400;
        internal const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
        internal const uint SYNCHRONIZE = 0x00100000;
        internal const uint WAIT_OBJECT_0 = 0x00000000;
        internal const uint WAIT_TIMEOUT = 0x00000102;
        internal const uint STILL_ACTIVE = 259;
        internal const uint TH32CS_SNAPPROCESS = 0x00000002;
        internal const uint LIST_MODULES_ALL = 0x03;
        internal const uint WM_CLOSE = 0x0010;
        internal const uint FILE_SHARE_READ = 0x00000001;
        internal const uint FILE_SHARE_WRITE = 0x00000002;
        internal const uint FILE_SHARE_DELETE = 0x00000004;
        internal const uint OPEN_EXISTING = 3;
        internal const uint FILE_ATTRIBUTE_NORMAL = 0x00000080;
        internal const uint FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;

        [StructLayout(LayoutKind.Sequential)]
        internal struct FILETIME
        {
            internal uint Low;
            internal uint High;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct PROCESSENTRY32
        {
            internal uint dwSize;
            internal uint cntUsage;
            internal uint th32ProcessID;
            internal IntPtr th32DefaultHeapID;
            internal uint th32ModuleID;
            internal uint cntThreads;
            internal uint th32ParentProcessID;
            internal int pcPriClassBase;
            internal uint dwFlags;

            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            internal string szExeFile;
        }

        internal delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern SafeProcessHandle OpenProcess(
            uint desiredAccess,
            bool inheritHandle,
            int processId);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool CloseHandle(IntPtr handle);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool QueryFullProcessImageName(
            SafeProcessHandle process,
            int flags,
            StringBuilder imageName,
            ref int size);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetProcessTimes(
            SafeProcessHandle process,
            out FILETIME creation,
            out FILETIME exit,
            out FILETIME kernel,
            out FILETIME user);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint WaitForSingleObject(SafeProcessHandle handle, uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetExitCodeProcess(SafeProcessHandle process, out uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool TerminateProcess(SafeProcessHandle process, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern SafeFileHandle CreateFile(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            IntPtr securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern uint GetFinalPathNameByHandle(
            SafeFileHandle file,
            StringBuilder path,
            uint pathLength,
            uint flags);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool Process32First(IntPtr snapshot, ref PROCESSENTRY32 entry);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool Process32Next(IntPtr snapshot, ref PROCESSENTRY32 entry);

        [DllImport("psapi.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool EnumProcessModulesEx(
            SafeProcessHandle process,
            [Out] IntPtr[] modules,
            uint bytes,
            out uint bytesNeeded,
            uint filterFlag);

        [DllImport("psapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern uint GetModuleFileNameEx(
            SafeProcessHandle process,
            IntPtr module,
            StringBuilder fileName,
            uint size);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);

        [DllImport("user32.dll")]
        internal static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    }

    internal static class WindowsProcess
    {
        private static BoundaryException Win32Failure(string code, string action)
        {
            int error = Marshal.GetLastWin32Error();
            return new BoundaryException(
                code,
                action + " failed with Win32 error " + error.ToString(CultureInfo.InvariantCulture) +
                ": " + new Win32Exception(error).Message);
        }

        private static long ToLong(NativeMethods.FILETIME value)
        {
            return unchecked((long)(((ulong)value.High << 32) | value.Low));
        }

        internal static string ToIso(long creationFileTime)
        {
            return DateTime.FromFileTimeUtc(creationFileTime).ToString("o", CultureInfo.InvariantCulture);
        }

        internal static string CanonicalizeExistingPath(string path, bool directory)
        {
            string fullPath;
            try
            {
                fullPath = Path.GetFullPath(path);
            }
            catch (Exception exception)
            {
                throw new BoundaryException("INVALID_PATH", "The path is invalid: " + exception.Message);
            }

            uint attributes = directory
                ? NativeMethods.FILE_FLAG_BACKUP_SEMANTICS
                : NativeMethods.FILE_ATTRIBUTE_NORMAL;
            using (SafeFileHandle handle = NativeMethods.CreateFile(
                fullPath,
                0,
                NativeMethods.FILE_SHARE_READ | NativeMethods.FILE_SHARE_WRITE | NativeMethods.FILE_SHARE_DELETE,
                IntPtr.Zero,
                NativeMethods.OPEN_EXISTING,
                attributes,
                IntPtr.Zero))
            {
                if (handle.IsInvalid)
                {
                    throw Win32Failure("PATH_ACCESS_FAILED", "Opening " + fullPath);
                }

                StringBuilder builder = new StringBuilder(32768);
                uint length = NativeMethods.GetFinalPathNameByHandle(
                    handle,
                    builder,
                    (uint)builder.Capacity,
                    0);
                if (length == 0)
                {
                    throw Win32Failure("PATH_CANONICALIZATION_FAILED", "Canonicalizing " + fullPath);
                }
                if (length >= builder.Capacity)
                {
                    throw new BoundaryException(
                        "PATH_TOO_LONG",
                        "The canonical path exceeds the supported Windows path length.");
                }

                string result = builder.ToString();
                if (result.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
                {
                    result = @"\\" + result.Substring(8);
                }
                else if (result.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase))
                {
                    result = result.Substring(4);
                }

                result = Path.GetFullPath(result);
                if (directory)
                {
                    string root = Path.GetPathRoot(result);
                    if (!String.Equals(root, result, StringComparison.OrdinalIgnoreCase))
                    {
                        result = result.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                    }
                }
                return result;
            }
        }

        internal static SafeProcessHandle OpenProcessHandle(int pid, uint access, string code)
        {
            SafeProcessHandle handle = NativeMethods.OpenProcess(access, false, pid);
            if (handle == null || handle.IsInvalid)
            {
                if (handle != null)
                {
                    handle.Dispose();
                }
                throw Win32Failure(code, "Opening process " + pid.ToString(CultureInfo.InvariantCulture));
            }
            return handle;
        }

        internal static bool IsRunning(SafeProcessHandle handle)
        {
            uint wait = NativeMethods.WaitForSingleObject(handle, 0);
            if (wait == NativeMethods.WAIT_TIMEOUT)
            {
                return true;
            }
            if (wait == NativeMethods.WAIT_OBJECT_0)
            {
                return false;
            }
            throw Win32Failure("PROCESS_WAIT_FAILED", "Checking the retained process handle");
        }

        internal static string QueryImagePath(SafeProcessHandle handle)
        {
            StringBuilder builder = new StringBuilder(32768);
            int size = builder.Capacity;
            if (!NativeMethods.QueryFullProcessImageName(handle, 0, builder, ref size))
            {
                throw Win32Failure("PROCESS_QUERY_FAILED", "Querying the process image path");
            }
            return CanonicalizeExistingPath(builder.ToString(), false);
        }

        internal static long QueryCreationFileTime(SafeProcessHandle handle)
        {
            NativeMethods.FILETIME creation;
            NativeMethods.FILETIME exit;
            NativeMethods.FILETIME kernel;
            NativeMethods.FILETIME user;
            if (!NativeMethods.GetProcessTimes(handle, out creation, out exit, out kernel, out user))
            {
                throw Win32Failure("PROCESS_QUERY_FAILED", "Querying the process creation time");
            }
            return ToLong(creation);
        }

        internal static Dictionary<int, int> SnapshotParents()
        {
            IntPtr snapshot = NativeMethods.CreateToolhelp32Snapshot(NativeMethods.TH32CS_SNAPPROCESS, 0);
            if (snapshot == new IntPtr(-1))
            {
                throw Win32Failure("PROCESS_SNAPSHOT_FAILED", "Creating a process snapshot");
            }

            try
            {
                Dictionary<int, int> result = new Dictionary<int, int>();
                NativeMethods.PROCESSENTRY32 entry = new NativeMethods.PROCESSENTRY32();
                entry.dwSize = (uint)Marshal.SizeOf(typeof(NativeMethods.PROCESSENTRY32));
                if (!NativeMethods.Process32First(snapshot, ref entry))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 18)
                    {
                        return result;
                    }
                    throw Win32Failure("PROCESS_SNAPSHOT_FAILED", "Reading the process snapshot");
                }

                do
                {
                    result[unchecked((int)entry.th32ProcessID)] =
                        unchecked((int)entry.th32ParentProcessID);
                    entry.dwSize = (uint)Marshal.SizeOf(typeof(NativeMethods.PROCESSENTRY32));
                }
                while (NativeMethods.Process32Next(snapshot, ref entry));

                int finalError = Marshal.GetLastWin32Error();
                if (finalError != 18)
                {
                    throw Win32Failure("PROCESS_SNAPSHOT_FAILED", "Reading the process snapshot");
                }
                return result;
            }
            finally
            {
                NativeMethods.CloseHandle(snapshot);
            }
        }

        internal static List<ProcessIdentity> FindExactImage(string canonicalImage)
        {
            string targetName = Path.GetFileName(canonicalImage);
            IntPtr snapshot = NativeMethods.CreateToolhelp32Snapshot(NativeMethods.TH32CS_SNAPPROCESS, 0);
            if (snapshot == new IntPtr(-1))
            {
                throw Win32Failure("PROCESS_SNAPSHOT_FAILED", "Creating a process snapshot");
            }

            try
            {
                List<ProcessIdentity> result = new List<ProcessIdentity>();
                NativeMethods.PROCESSENTRY32 entry = new NativeMethods.PROCESSENTRY32();
                entry.dwSize = (uint)Marshal.SizeOf(typeof(NativeMethods.PROCESSENTRY32));
                if (!NativeMethods.Process32First(snapshot, ref entry))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 18)
                    {
                        return result;
                    }
                    throw Win32Failure("PROCESS_SNAPSHOT_FAILED", "Reading the process snapshot");
                }

                do
                {
                    if (String.Equals(entry.szExeFile, targetName, StringComparison.OrdinalIgnoreCase))
                    {
                        int pid = unchecked((int)entry.th32ProcessID);
                        SafeProcessHandle handle = NativeMethods.OpenProcess(
                            NativeMethods.PROCESS_QUERY_LIMITED_INFORMATION | NativeMethods.SYNCHRONIZE,
                            false,
                            pid);
                        if (handle == null || handle.IsInvalid)
                        {
                            int error = Marshal.GetLastWin32Error();
                            if (handle != null)
                            {
                                handle.Dispose();
                            }
                            if (error != 87 && error != 1168 && error != 6)
                            {
                                throw new BoundaryException(
                                    "PROCESS_ACCESS_DENIED",
                                    "A process named " + targetName + " could not be identified exactly. " +
                                    "Win32 error " + error.ToString(CultureInfo.InvariantCulture) + ": " +
                                    new Win32Exception(error).Message);
                            }
                        }
                        else
                        {
                            using (handle)
                            {
                                if (IsRunning(handle))
                                {
                                    string image = QueryImagePath(handle);
                                    if (String.Equals(image, canonicalImage, StringComparison.OrdinalIgnoreCase))
                                    {
                                        long creation = QueryCreationFileTime(handle);
                                        result.Add(new ProcessIdentity {
                                            Pid = pid,
                                            ParentPid = unchecked((int)entry.th32ParentProcessID),
                                            CreationFileTime = creation,
                                            CreationTime = ToIso(creation),
                                            ImagePath = image
                                        });
                                    }
                                }
                            }
                        }
                    }

                    entry.dwSize = (uint)Marshal.SizeOf(typeof(NativeMethods.PROCESSENTRY32));
                }
                while (NativeMethods.Process32Next(snapshot, ref entry));

                int finalError = Marshal.GetLastWin32Error();
                if (finalError != 18)
                {
                    throw Win32Failure("PROCESS_SNAPSHOT_FAILED", "Reading the process snapshot");
                }
                return result;
            }
            finally
            {
                NativeMethods.CloseHandle(snapshot);
            }
        }

        internal static ProcessIdentity TryGetIdentity(int pid)
        {
            SafeProcessHandle handle = NativeMethods.OpenProcess(
                NativeMethods.PROCESS_QUERY_LIMITED_INFORMATION | NativeMethods.SYNCHRONIZE,
                false,
                pid);
            if (handle == null || handle.IsInvalid)
            {
                int error = Marshal.GetLastWin32Error();
                if (handle != null)
                {
                    handle.Dispose();
                }
                if (error == 87 || error == 1168 || error == 6)
                {
                    return null;
                }
                throw new BoundaryException(
                    "PROCESS_ACCESS_DENIED",
                    "Process " + pid.ToString(CultureInfo.InvariantCulture) +
                    " could not be identified. Win32 error " +
                    error.ToString(CultureInfo.InvariantCulture) + ": " +
                    new Win32Exception(error).Message);
            }

            using (handle)
            {
                if (!IsRunning(handle))
                {
                    return null;
                }
                long creation = QueryCreationFileTime(handle);
                return new ProcessIdentity {
                    Pid = pid,
                    CreationFileTime = creation,
                    CreationTime = ToIso(creation),
                    ImagePath = QueryImagePath(handle)
                };
            }
        }

        internal static SafeProcessHandle RetainOwnedProcess(ProcessIdentity identity)
        {
            SafeProcessHandle handle = OpenProcessHandle(
                identity.Pid,
                NativeMethods.PROCESS_TERMINATE |
                NativeMethods.PROCESS_QUERY_LIMITED_INFORMATION |
                NativeMethods.SYNCHRONIZE,
                "OWNED_PROCESS_OPEN_FAILED");
            try
            {
                if (!IsRunning(handle))
                {
                    throw new BoundaryException(
                        "GAME_EXITED_DURING_START",
                        "The proven game process exited before its handle could be retained.");
                }
                string image = QueryImagePath(handle);
                long creation = QueryCreationFileTime(handle);
                if (!String.Equals(image, identity.ImagePath, StringComparison.OrdinalIgnoreCase) ||
                    creation != identity.CreationFileTime)
                {
                    throw new BoundaryException(
                        "PROCESS_IDENTITY_CHANGED",
                        "The game PID was reused before ownership could be retained.");
                }
                return handle;
            }
            catch
            {
                handle.Dispose();
                throw;
            }
        }

        internal static bool RevalidateOwnedAfterRunningObserved(OwnedSession session)
        {
            string image;
            long creation;
            try
            {
                image = QueryImagePath(session.Handle);
                creation = QueryCreationFileTime(session.Handle);
            }
            catch (BoundaryException)
            {
                // QueryFullProcessImageName can fail after the process exits even though
                // the immediately preceding retained-handle wait still reported it live.
                // Only the retained handle becoming signaled proves that this is an exit
                // race; a query failure while it remains live must still be surfaced.
                if (!IsRunning(session.Handle))
                {
                    return false;
                }
                throw;
            }

            if (!IsRunning(session.Handle))
            {
                return false;
            }
            if (!String.Equals(image, session.ImagePath, StringComparison.OrdinalIgnoreCase) ||
                creation != session.CreationFileTime)
            {
                throw new BoundaryException(
                    "OWNED_PROCESS_IDENTITY_CHANGED",
                    "The retained game handle no longer matches its recorded identity.");
            }
            return true;
        }

        internal static bool RevalidateOwned(OwnedSession session)
        {
            if (!IsRunning(session.Handle))
            {
                return false;
            }
            return RevalidateOwnedAfterRunningObserved(session);
        }

        internal static int RequestClose(int pid)
        {
            int matched = 0;
            int posted = 0;
            NativeMethods.EnumWindowsCallback callback = delegate(IntPtr window, IntPtr parameter)
            {
                uint windowPid;
                NativeMethods.GetWindowThreadProcessId(window, out windowPid);
                if (windowPid == unchecked((uint)pid))
                {
                    matched++;
                    if (NativeMethods.PostMessage(
                        window,
                        NativeMethods.WM_CLOSE,
                        IntPtr.Zero,
                        IntPtr.Zero))
                    {
                        posted++;
                    }
                }
                return true;
            };

            if (!NativeMethods.EnumWindows(callback, IntPtr.Zero))
            {
                throw Win32Failure("WINDOW_ENUM_FAILED", "Enumerating top-level windows");
            }
            if (matched != 0 && posted == 0)
            {
                throw Win32Failure("WINDOW_CLOSE_FAILED", "Posting WM_CLOSE to the owned game window");
            }
            return posted;
        }

        internal static bool WaitForExit(SafeProcessHandle handle, int timeoutMs)
        {
            uint wait = NativeMethods.WaitForSingleObject(handle, unchecked((uint)timeoutMs));
            if (wait == NativeMethods.WAIT_OBJECT_0)
            {
                return true;
            }
            if (wait == NativeMethods.WAIT_TIMEOUT)
            {
                return false;
            }
            throw Win32Failure("PROCESS_WAIT_FAILED", "Waiting for the owned game process");
        }

        internal static void Terminate(SafeProcessHandle handle)
        {
            if (!NativeMethods.TerminateProcess(handle, 1))
            {
                throw Win32Failure("TERMINATE_FAILED", "Terminating the retained owned game process");
            }
        }

        internal static string FindModulePath(SafeProcessHandle handle, string moduleName)
        {
            IntPtr[] modules = new IntPtr[256];
            uint bytesNeeded;
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                if (!NativeMethods.EnumProcessModulesEx(
                    handle,
                    modules,
                    unchecked((uint)(modules.Length * IntPtr.Size)),
                    out bytesNeeded,
                    NativeMethods.LIST_MODULES_ALL))
                {
                    throw Win32Failure(
                        "MODULE_ENUM_FAILED",
                        "Enumerating modules in the requested process");
                }

                int count = unchecked((int)(bytesNeeded / (uint)IntPtr.Size));
                if (count <= modules.Length)
                {
                    string found = null;
                    for (int index = 0; index < count; ++index)
                    {
                        StringBuilder path = new StringBuilder(32768);
                        uint length = NativeMethods.GetModuleFileNameEx(
                            handle,
                            modules[index],
                            path,
                            unchecked((uint)path.Capacity));
                        if (length == 0)
                        {
                            throw Win32Failure(
                                "MODULE_QUERY_FAILED",
                                "Reading a loaded module path");
                        }

                        string candidate = path.ToString();
                        if (String.Equals(
                            Path.GetFileName(candidate),
                            moduleName,
                            StringComparison.OrdinalIgnoreCase))
                        {
                            string canonical = CanonicalizeExistingPath(candidate, false);
                            if (found != null &&
                                !String.Equals(found, canonical, StringComparison.OrdinalIgnoreCase))
                            {
                                throw new BoundaryException(
                                    "AMBIGUOUS_PLUGIN_MODULE",
                                    "More than one loaded module matches " + moduleName + ".");
                            }
                            found = canonical;
                        }
                    }
                    return found;
                }

                modules = new IntPtr[count + 32];
            }

            throw new BoundaryException(
                "MODULE_ENUM_UNSTABLE",
                "The loaded module list changed repeatedly while it was being inspected.");
        }

        internal static string Sha256(string path)
        {
            try
            {
                using (FileStream stream = new FileStream(
                    path,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete))
                using (SHA256 algorithm = SHA256.Create())
                {
                    byte[] hash = algorithm.ComputeHash(stream);
                    StringBuilder text = new StringBuilder(hash.Length * 2);
                    for (int index = 0; index < hash.Length; ++index)
                    {
                        text.Append(hash[index].ToString("x2", CultureInfo.InvariantCulture));
                    }
                    return text.ToString();
                }
            }
            catch (Exception exception)
            {
                throw new BoundaryException(
                    "PLUGIN_HASH_FAILED",
                    "Hashing the requested plugin module failed: " + exception.Message);
            }
        }
    }

    public sealed class Controller : IDisposable
    {
        public static string ShimScriptPath { get; set; }
        private const int MaximumLineCharacters = 1024 * 1024;
        private const int MaximumStartTimeoutMs = 10 * 60 * 1000;
        private const int MaximumCloseTimeoutMs = 5 * 60 * 1000;
        private const int TerminateWaitMs = 5000;
        private Mutex lease_;
        private bool leaseHeld_;
        private string leaseGameImage_;
        private bool unresolvedLaunchAttempt_;
        private OwnedSession owned_;

        public Controller()
        {
        }

        public static int MaximumInputLineCharacters
        {
            get { return MaximumLineCharacters; }
        }

        public object HandleOversizedLine()
        {
            return CreateFailure(
                null,
                "LINE_TOO_LONG",
                "The request exceeds the 1048576-character JSONL limit.");
        }

        public object HandleRequest(Dictionary<string, object> request)
        {
            object id = null;
            try
            {
                id = RequirePositiveInteger(request, "id");
                string operation = RequireString(request, "op");
                object result;

                switch (operation)
                {
                    case "start":
                        result = Start(request);
                        break;
                    case "status":
                        result = Status();
                        break;
                    case "inspect":
                        result = Inspect(request);
                        break;
                    case "close":
                        result = Close(request);
                        break;
                    case "terminate":
                        result = Terminate();
                        break;
                    case "release":
                        result = Release();
                        break;
                    default:
                        throw new BoundaryException(
                            "UNKNOWN_OPERATION",
                            "Unsupported operation: " + operation);
                }

                Dictionary<string, object> response = new Dictionary<string, object>();
                response["id"] = id;
                response["ok"] = true;
                response["result"] = result;
                return response;
            }
            catch (BoundaryException exception)
            {
                return CreateFailure(id, exception.Code, exception.Message);
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine(
                    "[windows-session] Unexpected request failure: " + exception.ToString());
                return CreateFailure(id, "INTERNAL_ERROR", exception.Message);
            }
        }

        public static object CreateFailure(object id, string code, string message)
        {
            Dictionary<string, object> error = new Dictionary<string, object>();
            error["code"] = code;
            error["message"] = message;

            Dictionary<string, object> response = new Dictionary<string, object>();
            response["id"] = id;
            response["ok"] = false;
            response["error"] = error;
            return response;
        }

        private static long RequirePositiveInteger(
            Dictionary<string, object> request,
            string field)
        {
            object value;
            if (!request.TryGetValue(field, out value) || value == null)
            {
                throw new BoundaryException(
                    "INVALID_REQUEST",
                    "Field " + field + " must be a positive integer.");
            }

            try
            {
                decimal decimalValue = Convert.ToDecimal(value, CultureInfo.InvariantCulture);
                long integerValue = Convert.ToInt64(value, CultureInfo.InvariantCulture);
                if (decimalValue != integerValue || integerValue <= 0)
                {
                    throw new BoundaryException(
                        "INVALID_REQUEST",
                        "Field " + field + " must be a positive integer.");
                }
                return integerValue;
            }
            catch (BoundaryException)
            {
                throw;
            }
            catch (Exception)
            {
                throw new BoundaryException(
                    "INVALID_REQUEST",
                    "Field " + field + " must be a positive integer.");
            }
        }

        private static int RequireTimeout(
            Dictionary<string, object> request,
            string field,
            int maximum)
        {
            long value = RequirePositiveInteger(request, field);
            if (value > maximum)
            {
                throw new BoundaryException(
                    "INVALID_TIMEOUT",
                    "Field " + field + " exceeds the supported maximum of " +
                    maximum.ToString(CultureInfo.InvariantCulture) + " milliseconds.");
            }
            return unchecked((int)value);
        }

        private static int RequirePid(Dictionary<string, object> request)
        {
            long value = RequirePositiveInteger(request, "pid");
            if (value > Int32.MaxValue)
            {
                throw new BoundaryException("INVALID_REQUEST", "Field pid is outside the Windows PID range.");
            }
            return unchecked((int)value);
        }

        private static string RequireString(
            Dictionary<string, object> request,
            string field)
        {
            object value;
            if (!request.TryGetValue(field, out value))
            {
                throw new BoundaryException(
                    "INVALID_REQUEST",
                    "Field " + field + " must be a non-empty string.");
            }
            string text = value as string;
            if (String.IsNullOrWhiteSpace(text) ||
                text.IndexOf('\0') >= 0 ||
                text.IndexOf('\r') >= 0 ||
                text.IndexOf('\n') >= 0)
            {
                throw new BoundaryException(
                    "INVALID_REQUEST",
                    "Field " + field + " must be a non-empty single-line string.");
            }
            return text;
        }

        private static string OptionalString(
            Dictionary<string, object> request,
            string field,
            string defaultValue)
        {
            object value;
            if (!request.TryGetValue(field, out value) || value == null)
            {
                return defaultValue;
            }
            string text = value as string;
            if (String.IsNullOrWhiteSpace(text) ||
                text.IndexOf('\0') >= 0 ||
                text.IndexOf('\r') >= 0 ||
                text.IndexOf('\n') >= 0)
            {
                throw new BoundaryException(
                    "INVALID_REQUEST",
                    "Field " + field + " must be a non-empty single-line string.");
            }
            return text;
        }

        private static string ValidateFile(string rawPath, string field, string code)
        {
            if (!File.Exists(rawPath))
            {
                throw new BoundaryException(code, "Field " + field + " does not name an existing file.");
            }
            try
            {
                return WindowsProcess.CanonicalizeExistingPath(rawPath, false);
            }
            catch (BoundaryException exception)
            {
                throw new BoundaryException(code, "Field " + field + " is not usable: " + exception.Message);
            }
        }

        private static string ValidateDirectory(string rawPath, string field, string code)
        {
            if (!Directory.Exists(rawPath))
            {
                throw new BoundaryException(code, "Field " + field + " does not name an existing directory.");
            }
            try
            {
                return WindowsProcess.CanonicalizeExistingPath(rawPath, true);
            }
            catch (BoundaryException exception)
            {
                throw new BoundaryException(code, "Field " + field + " is not usable: " + exception.Message);
            }
        }

        private static string ValidateProfile(string profilesDirectory, string profile)
        {
            if (Path.IsPathRooted(profile) ||
                profile == "." ||
                profile == ".." ||
                profile.IndexOf(Path.DirectorySeparatorChar) >= 0 ||
                profile.IndexOf(Path.AltDirectorySeparatorChar) >= 0 ||
                !String.Equals(Path.GetFileName(profile), profile, StringComparison.Ordinal))
            {
                throw new BoundaryException(
                    "INVALID_PROFILE",
                    "Field profile must be one direct profile folder name, not a path.");
            }

            string candidate = Path.Combine(profilesDirectory, profile);
            if (!Directory.Exists(candidate))
            {
                throw new BoundaryException(
                    "PROFILE_NOT_FOUND",
                    "The selected Mod Organizer profile folder does not exist.");
            }

            string canonical;
            try
            {
                canonical = WindowsProcess.CanonicalizeExistingPath(candidate, true);
            }
            catch (BoundaryException exception)
            {
                throw new BoundaryException(
                    "INVALID_PROFILE",
                    "The selected profile folder is not usable: " + exception.Message);
            }

            DirectoryInfo parent = Directory.GetParent(canonical);
            if (parent == null ||
                !String.Equals(parent.FullName, profilesDirectory, StringComparison.OrdinalIgnoreCase))
            {
                throw new BoundaryException(
                    "INVALID_PROFILE",
                    "The selected profile must resolve to a direct child of profilesDir.");
            }
            return canonical;
        }

        private static string DecodeIniValue(string value)
        {
            string trimmed = value.Trim();
            if (trimmed.Length >= 2 && trimmed[0] == '"' && trimmed[trimmed.Length - 1] == '"')
                trimmed = trimmed.Substring(1, trimmed.Length - 2);
            if (trimmed.StartsWith("@ByteArray(", StringComparison.Ordinal) &&
                trimmed.EndsWith(")", StringComparison.Ordinal))
            {
                return trimmed.Substring(11, trimmed.Length - 12);
            }
            return trimmed.Replace("\\\"", "\"").Replace("\\\\", "\\");
        }

        private static void ValidateExecutableLabel(string mo2Executable, string executable, string gameImage)
        {
            string iniPath = Path.Combine(Path.GetDirectoryName(mo2Executable), "ModOrganizer.ini");
            if (!File.Exists(iniPath))
            {
                throw new BoundaryException("MO2_CONFIG_NOT_FOUND",
                    "The non-admin launcher requires the portable MO2 instance's adjacent ModOrganizer.ini.");
            }

            string[] lines;
            try
            {
                lines = File.ReadAllLines(iniPath);
            }
            catch (Exception exception)
            {
                throw new BoundaryException(
                    "MO2_CONFIG_ACCESS_FAILED",
                    "ModOrganizer.ini exists but could not be read: " + exception.Message);
            }

            bool inCustomExecutables = false;
            Dictionary<string, Dictionary<string, string>> entries =
                new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
            foreach (string rawLine in lines)
            {
                string line = rawLine.Trim();
                if (line.Length == 0 || line.StartsWith(";", StringComparison.Ordinal) ||
                    line.StartsWith("#", StringComparison.Ordinal))
                {
                    continue;
                }
                if (line.StartsWith("[", StringComparison.Ordinal) &&
                    line.EndsWith("]", StringComparison.Ordinal))
                {
                    string section = line.Substring(1, line.Length - 2).Trim();
                    inCustomExecutables = String.Equals(
                        section,
                        "customExecutables",
                        StringComparison.OrdinalIgnoreCase);
                    continue;
                }

                int separator = line.IndexOf('=');
                if (separator <= 0)
                {
                    continue;
                }
                string key = line.Substring(0, separator).Trim();
                string value = DecodeIniValue(line.Substring(separator + 1));
                if (key.StartsWith(@"customExecutables\", StringComparison.OrdinalIgnoreCase))
                    key = key.Substring("customExecutables\\".Length);
                else if (!inCustomExecutables)
                    continue;
                int split = key.IndexOf('\\');
                if (split <= 0)
                    continue;
                string index = key.Substring(0, split);
                Dictionary<string, string> entry;
                if (!entries.TryGetValue(index, out entry))
                {
                    entry = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    entries.Add(index, entry);
                }
                entry[key.Substring(split + 1)] = value;
            }

            Dictionary<string, string> selected = null;
            foreach (Dictionary<string, string> entry in entries.Values)
            {
                string title;
                if (!entry.TryGetValue("title", out title) ||
                    !String.Equals(title, executable, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (selected != null)
                    throw new BoundaryException("INVALID_SHIM_CONFIG", "The MO2 executable title is ambiguous.");
                selected = entry;
            }
            if (selected == null)
            {
                throw new BoundaryException(
                    "EXECUTABLE_NOT_CONFIGURED",
                    "The requested executable label is not present in the adjacent ModOrganizer.ini.");
            }
            string binary, command;
            if (!selected.TryGetValue("binary", out binary) || !selected.TryGetValue("arguments", out command))
                throw new BoundaryException("INVALID_SHIM_CONFIG", "The MO2 executable must specify the DevBench shim.");
            string shim = Path.Combine(Path.GetDirectoryName(ShimScriptPath), "devbench-launch.exe");
            if (!String.Equals(ValidateFile(binary, "binary", "INVALID_SHIM_CONFIG"),
                ValidateFile(shim, "shim", "INVALID_SHIM_CONFIG"),
                StringComparison.OrdinalIgnoreCase))
                throw new BoundaryException("INVALID_SHIM_CONFIG", "The MO2 entry must use the packaged native DevBench launcher.");
            string[] args = ShimNative.Arguments(command);
            if (args.Length != 2 ||
                !String.Equals(args[0], "-GameExe", StringComparison.OrdinalIgnoreCase) ||
                !String.Equals(ValidateFile(args[1], "game", "INVALID_SHIM_CONFIG"), gameImage,
                    StringComparison.OrdinalIgnoreCase))
                throw new BoundaryException("INVALID_SHIM_CONFIG",
                    "Expected native shim arguments: -GameExe <gameExe>.");
        }

        internal static string QuoteWindowsArgument(string argument)
        {
            if (argument.Length != 0 &&
                argument.IndexOfAny(new char[] { ' ', '\t', '\v', '"' }) < 0)
            {
                return argument;
            }

            StringBuilder builder = new StringBuilder();
            builder.Append('"');
            int backslashes = 0;
            foreach (char character in argument)
            {
                if (character == '\\')
                {
                    backslashes++;
                    continue;
                }
                if (character == '"')
                {
                    builder.Append('\\', backslashes * 2 + 1);
                    builder.Append('"');
                    backslashes = 0;
                    continue;
                }

                builder.Append('\\', backslashes);
                backslashes = 0;
                builder.Append(character);
            }
            builder.Append('\\', backslashes * 2);
            builder.Append('"');
            return builder.ToString();
        }

        private static string LeaseName(string canonicalGameImage)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(canonicalGameImage.ToUpperInvariant());
            using (SHA256 algorithm = SHA256.Create())
            {
                byte[] hash = algorithm.ComputeHash(bytes);
                StringBuilder text = new StringBuilder(hash.Length * 2);
                foreach (byte value in hash)
                {
                    text.Append(value.ToString("x2", CultureInfo.InvariantCulture));
                }
                return @"Local\BethesdaDevBench.Game." + text.ToString();
            }
        }

        private void AcquireLease(string canonicalGameImage)
        {
            if (leaseHeld_)
            {
                throw new BoundaryException(
                    "LEASE_ALREADY_HELD",
                    "This helper already holds a game-image lease.");
            }

            Mutex candidate;
            try
            {
                candidate = new Mutex(false, LeaseName(canonicalGameImage));
            }
            catch (Exception exception)
            {
                throw new BoundaryException(
                    "LEASE_CREATE_FAILED",
                    "Creating the Windows game-image lease failed: " + exception.Message);
            }

            bool acquired = false;
            try
            {
                try
                {
                    acquired = candidate.WaitOne(0);
                }
                catch (AbandonedMutexException)
                {
                    acquired = true;
                }

                if (!acquired)
                {
                    throw new BoundaryException(
                        "LEASE_BUSY",
                        "Another controller holds the lease for this exact game executable.");
                }

                lease_ = candidate;
                leaseHeld_ = true;
                leaseGameImage_ = canonicalGameImage;
                candidate = null;
            }
            finally
            {
                if (candidate != null)
                {
                    candidate.Dispose();
                }
            }
        }

        private void ReleaseLease()
        {
            if (!leaseHeld_)
            {
                if (lease_ != null)
                {
                    lease_.Dispose();
                    lease_ = null;
                }
                leaseGameImage_ = null;
                unresolvedLaunchAttempt_ = false;
                return;
            }

            try
            {
                lease_.ReleaseMutex();
            }
            catch (ApplicationException exception)
            {
                Console.Error.WriteLine(
                    "[windows-session] Releasing the game-image lease failed: " + exception.Message);
            }
            finally
            {
                leaseHeld_ = false;
                lease_.Dispose();
                lease_ = null;
                leaseGameImage_ = null;
                unresolvedLaunchAttempt_ = false;
            }
        }

        private void RefreshOwned()
        {
            if (owned_ == null || !owned_.Running)
            {
                return;
            }

            if (!WindowsProcess.RevalidateOwned(owned_))
            {
                owned_.Running = false;
                if (owned_.ShimHandle == null || !WindowsProcess.IsRunning(owned_.ShimHandle))
                {
                    owned_.Dispose();
                    ReleaseLease();
                }
                return;
            }
        }

        private object Start(Dictionary<string, object> request)
        {
            RefreshOwned();
            if (owned_ != null && owned_.Running)
            {
                throw new BoundaryException(
                    "OWNED_GAME_ALREADY_RUNNING",
                    "This helper already owns a running game process.");
            }
            if (unresolvedLaunchAttempt_ || leaseHeld_)
            {
                throw new BoundaryException(
                    "UNRESOLVED_LAUNCH_ATTEMPT",
                    "Release this helper after the prior unowned launch attempt before starting again.");
            }
            if (owned_ != null)
            {
                owned_.Dispose();
                owned_ = null;
            }

            string mo2Image = ValidateFile(
                RequireString(request, "mo2Exe"),
                "mo2Exe",
                "INVALID_MO2_PATH");
            string profilesDirectory = ValidateDirectory(
                RequireString(request, "profilesDir"),
                "profilesDir",
                "INVALID_PROFILES_PATH");
            string profile = RequireString(request, "profile");
            ValidateProfile(profilesDirectory, profile);
            string executable = RequireString(request, "executable");
            string gameImage = ValidateFile(
                RequireString(request, "gameExe"),
                "gameExe",
                "INVALID_GAME_PATH");
            string loader = RequireString(request, "loaderExe");
            if (!Path.IsPathFullyQualified(loader) ||
                !String.Equals(Path.GetDirectoryName(Path.GetFullPath(loader)),
                    Path.GetDirectoryName(gameImage), StringComparison.OrdinalIgnoreCase))
                throw new BoundaryException("INVALID_LOADER_PATH",
                    "loaderExe must be an absolute path beside gameExe; RootBuilder may supply it at launch.");
            int timeoutMs = RequireTimeout(request, "timeoutMs", MaximumStartTimeoutMs);
            ValidateExecutableLabel(mo2Image, executable, gameImage);

            List<ProcessIdentity> existingGames = WindowsProcess.FindExactImage(gameImage);
            if (existingGames.Count != 0)
            {
                throw new BoundaryException(
                    "GAME_ALREADY_RUNNING",
                    "The configured game executable is already running and cannot be adopted.");
            }

            List<ProcessIdentity> existingMo2List = WindowsProcess.FindExactImage(mo2Image);
            if (existingMo2List.Count > 1)
            {
                throw new BoundaryException(
                    "AMBIGUOUS_MO2_INSTANCE",
                    "More than one process is running from the configured Mod Organizer executable.");
            }
            AcquireLease(gameImage);
            bool launched = false;
            try
            {
                if (existingMo2List.Count != 1)
                    throw new BoundaryException("MO2_NOT_RUNNING",
                        "Open the configured MO2 instance first. The controller will not start, close, or restart MO2.");
                existingGames = WindowsProcess.FindExactImage(gameImage);
                if (existingGames.Count != 0)
                {
                    throw new BoundaryException(
                        "GAME_ALREADY_RUNNING",
                        "The configured game executable started before the launch lease was acquired.");
                }

                launched = true;
                unresolvedLaunchAttempt_ = true;
                owned_ = LaunchShim.Start(mo2Image, profile, executable, gameImage, loader, timeoutMs);
                unresolvedLaunchAttempt_ = false;
                RefreshOwned();
                return SessionResult(owned_, true);
            }
            catch (OperationCanceledException)
            {
                throw new BoundaryException("START_TIMEOUT",
                    "The non-admin shim did not complete ownership transfer before timeout. " +
                    "No unowned process was adopted or terminated; release the controller before another launch.");
            }
            catch
            {
                if (!launched && owned_ == null)
                {
                    ReleaseLease();
                }
                throw;
            }
        }

        private static Dictionary<string, object> SessionResult(
            OwnedSession session,
            bool includeRunning)
        {
            Dictionary<string, object> result = new Dictionary<string, object>();
            result["pid"] = session.Pid;
            result["creationTime"] = session.CreationTime;
            result["exe"] = session.ImagePath;
            result["mo2Pid"] = session.Mo2Pid;
            result["owned"] = true;
            if (includeRunning)
            {
                result["running"] = session.Running;
            }
            return result;
        }

        private object Status()
        {
            RefreshOwned();
            if (owned_ == null)
            {
                Dictionary<string, object> empty = new Dictionary<string, object>();
                empty["owned"] = false;
                empty["running"] = false;
                return empty;
            }
            return SessionResult(owned_, true);
        }

        private object Inspect(Dictionary<string, object> request)
        {
            int pid = RequirePid(request);
            string gameImage = ValidateFile(
                RequireString(request, "gameExe"),
                "gameExe",
                "INVALID_GAME_PATH");
            string pluginName = OptionalString(request, "pluginName", "devbench.dll");
            if (!String.Equals(Path.GetFileName(pluginName), pluginName, StringComparison.Ordinal) ||
                pluginName.IndexOf(Path.DirectorySeparatorChar) >= 0 ||
                pluginName.IndexOf(Path.AltDirectorySeparatorChar) >= 0)
            {
                throw new BoundaryException(
                    "INVALID_PLUGIN_NAME",
                    "Field pluginName must be a module file name, not a path.");
            }

            using (SafeProcessHandle handle = WindowsProcess.OpenProcessHandle(
                pid,
                NativeMethods.PROCESS_QUERY_INFORMATION |
                NativeMethods.PROCESS_QUERY_LIMITED_INFORMATION |
                NativeMethods.PROCESS_VM_READ |
                NativeMethods.SYNCHRONIZE,
                "PROCESS_OPEN_FAILED"))
            {
                if (!WindowsProcess.IsRunning(handle))
                {
                    throw new BoundaryException(
                        "PROCESS_EXITED",
                        "The requested process is not running.");
                }

                string image = WindowsProcess.QueryImagePath(handle);
                long creation = WindowsProcess.QueryCreationFileTime(handle);
                if (!String.Equals(image, gameImage, StringComparison.OrdinalIgnoreCase))
                {
                    throw new BoundaryException(
                        "PROCESS_PATH_MISMATCH",
                        "The requested PID is not running from the configured game executable.");
                }

                string runtimeVersion;
                try
                {
                    FileVersionInfo version = FileVersionInfo.GetVersionInfo(image);
                    runtimeVersion = !String.IsNullOrWhiteSpace(version.FileVersion)
                        ? version.FileVersion
                        : version.ProductVersion;
                }
                catch (Exception exception)
                {
                    throw new BoundaryException(
                        "VERSION_QUERY_FAILED",
                        "Reading the game executable version failed: " + exception.Message);
                }
                if (String.IsNullOrWhiteSpace(runtimeVersion))
                {
                    throw new BoundaryException(
                        "VERSION_UNAVAILABLE",
                        "The game executable does not expose a file or product version.");
                }

                string modulePath = WindowsProcess.FindModulePath(handle, pluginName);
                object plugin = null;
                if (modulePath != null)
                {
                    Dictionary<string, object> pluginResult = new Dictionary<string, object>();
                    pluginResult["path"] = modulePath;
                    pluginResult["sha256"] = WindowsProcess.Sha256(modulePath);
                    plugin = pluginResult;
                }

                Dictionary<string, object> result = new Dictionary<string, object>();
                result["pid"] = pid;
                result["creationTime"] = WindowsProcess.ToIso(creation);
                result["exe"] = image;
                result["running"] = true;
                result["runtimeVersion"] = runtimeVersion;
                result["plugin"] = plugin;
                return result;
            }
        }

        private bool FinishOwnedExit(int timeoutMs)
        {
            owned_.Running = false;
            if (owned_.ShimHandle != null && !WindowsProcess.WaitForExit(owned_.ShimHandle, timeoutMs))
                return false;
            owned_.Dispose();
            ReleaseLease();
            return true;
        }

        private object Close(Dictionary<string, object> request)
        {
            int timeoutMs = RequireTimeout(request, "timeoutMs", MaximumCloseTimeoutMs);
            RefreshOwned();
            if (owned_ == null)
            {
                throw new BoundaryException(
                    "NO_OWNED_GAME",
                    "There is no retained owned game process to close.");
            }
            if (!owned_.Running)
            {
                Dictionary<string, object> alreadyExited = SessionResult(owned_, false);
                alreadyExited["exited"] = FinishOwnedExit(timeoutMs);
                return alreadyExited;
            }

            if (!WindowsProcess.RevalidateOwned(owned_))
            {
                owned_.Running = false;
                Dictionary<string, object> exitedDuringValidation =
                    SessionResult(owned_, false);
                exitedDuringValidation["exited"] = FinishOwnedExit(timeoutMs);
                return exitedDuringValidation;
            }
            WindowsProcess.RequestClose(owned_.Pid);
            bool exited = WindowsProcess.WaitForExit(owned_.Handle, timeoutMs);
            if (exited)
            {
                exited = FinishOwnedExit(timeoutMs);
            }

            Dictionary<string, object> result = SessionResult(owned_, false);
            result["exited"] = exited;
            if (!exited)
            {
                result["reason"] = "timeout";
            }
            return result;
        }

        private object Terminate()
        {
            RefreshOwned();
            if (owned_ == null)
            {
                throw new BoundaryException(
                    "NO_OWNED_GAME",
                    "There is no retained owned game process to terminate.");
            }
            if (owned_.Running)
            {
                if (!WindowsProcess.RevalidateOwned(owned_))
                {
                    owned_.Running = false;
                }
            }
            if (owned_.Running)
            {
                WindowsProcess.Terminate(owned_.Handle);
                if (!WindowsProcess.WaitForExit(owned_.Handle, TerminateWaitMs))
                {
                    Dictionary<string, object> timedOut = SessionResult(owned_, false);
                    timedOut["exited"] = false;
                    timedOut["reason"] = "timeout";
                    return timedOut;
                }
            }

            Dictionary<string, object> result = SessionResult(owned_, false);
            result["exited"] = FinishOwnedExit(TerminateWaitMs);
            return result;
        }

        private object Release()
        {
            RefreshOwned();
            if (owned_ != null && !owned_.Running && !FinishOwnedExit(TerminateWaitMs))
                throw new BoundaryException("SHIM_EXIT_TIMEOUT",
                    "Fallout has exited but its launch shim is still completing. MO2 was not closed.");
            Dictionary<string, object> result = new Dictionary<string, object>();
            result["released"] = true;
            if (owned_ != null)
            {
                result["pid"] = owned_.Pid;
                result["running"] = owned_.Running;
                owned_.Dispose();
                owned_ = null;
            }
            else
            {
                result["running"] = false;
            }
            ReleaseLease();
            return result;
        }

        public void Dispose()
        {
            if (owned_ != null)
            {
                owned_.Dispose();
                owned_ = null;
            }
            ReleaseLease();
        }
    }
}
'@

try {
    Add-Type `
        -TypeDefinition ($source + [Environment]::NewLine + (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'LaunchShim.cs') -Raw)) `
        -Language CSharp `
        -ErrorAction Stop | Out-Null
}
catch {
    [Console]::Error.WriteLine(
        '[windows-session] Native helper initialization failed: ' + $_.Exception.Message)
    exit 2
}

[BethesdaDevBench.WindowsSession.Controller]::ShimScriptPath = (Resolve-Path -LiteralPath $PSCommandPath).Path

$controller = New-Object BethesdaDevBench.WindowsSession.Controller

function ConvertTo-NativeValue {
    param([Parameter(ValueFromPipeline = $true)] $Value)

    if ($null -eq $Value) {
        return $null
    }

    if ($Value -is [System.Management.Automation.PSCustomObject]) {
        $dictionary = New-Object 'System.Collections.Generic.Dictionary[string,object]'
        foreach ($property in $Value.PSObject.Properties) {
            $dictionary[$property.Name] = ConvertTo-NativeValue $property.Value
        }
        return $dictionary
    }

    if ($Value -is [System.Collections.IDictionary]) {
        $dictionary = New-Object 'System.Collections.Generic.Dictionary[string,object]'
        foreach ($key in $Value.Keys) {
            $dictionary[[string]$key] = ConvertTo-NativeValue $Value[$key]
        }
        return $dictionary
    }

    if (($Value -is [System.Collections.IEnumerable]) -and
        -not ($Value -is [string])) {
        $values = New-Object 'System.Collections.Generic.List[object]'
        foreach ($item in $Value) {
            $values.Add((ConvertTo-NativeValue $item))
        }
        return $values.ToArray()
    }

    return $Value
}

try {
    while ($true) {
        $line = [BethesdaDevBench.WindowsSession.BoundedLineReader]::Read(
            [Console]::In,
            [BethesdaDevBench.WindowsSession.Controller]::MaximumInputLineCharacters)
        if ($line.EndOfStream) {
            break
        }

        if ($line.TooLong) {
            $response = $controller.HandleOversizedLine()
        }
        else {
            try {
                $parsed = ConvertFrom-Json -InputObject $line.Line -ErrorAction Stop
                if (-not ($parsed -is [System.Management.Automation.PSCustomObject]) -and
                    -not ($parsed -is [System.Collections.IDictionary])) {
                    $response = [BethesdaDevBench.WindowsSession.Controller]::CreateFailure(
                        $null,
                        'INVALID_REQUEST',
                        'The request must be a JSON object.')
                }
                else {
                    $request = ConvertTo-NativeValue $parsed
                    $response = $controller.HandleRequest($request)
                }
            }
            catch {
                $response = [BethesdaDevBench.WindowsSession.Controller]::CreateFailure(
                    $null,
                    'INVALID_JSON',
                    'The request is not valid JSON: ' + $_.Exception.Message)
            }
        }

        $json = ConvertTo-Json -InputObject $response -Compress -Depth 8
        [Console]::Out.WriteLine($json)
        [Console]::Out.Flush()
    }
}
finally {
    $controller.Dispose()
}
