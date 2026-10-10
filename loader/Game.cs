using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace aimwhere
{
    /// Launching CS2, waiting for it to finish loading, and LoadLibrary injection.
    internal static class Game
    {
        // The DLL resolves every one of these the moment it starts and gives up if one is missing, so it
        // is only injected once all of them are in the process (the same list as addresses::modules).
        private static readonly string[] RequiredModules =
        {
            "client.dll", "engine2.dll", "server.dll", "scenesystem.dll", "materialsystem2.dll",
            "rendersystemdx11.dll", "panorama.dll", "schemasystem.dll", "inputsystem.dll", "soundsystem.dll",
            "tier0.dll", "particles.dll", "resourcesystem.dll", "localize.dll", "meshsystem.dll",
            "filesystem_stdio.dll", "vphysics2.dll", "steam_api64.dll",
        };

        public static Process Find() => Process.GetProcessesByName("cs2").FirstOrDefault();

        public static void Launch() =>
            Process.Start(new ProcessStartInfo("steam://rungameid/730") { UseShellExecute = true });

        private static HashSet<string> ModuleNames(Process process)
        {
            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                foreach (ProcessModule m in process.Modules)
                    names.Add(m.ModuleName);
            }
            catch (Win32Exception) { } // the module list is mid-change while the game starts; next poll
            catch (InvalidOperationException) { }
            return names;
        }

        public static bool IsInjected(Process process) => ModuleNames(process).Contains("aimwhere.dll");

        public static bool IsElevated()
        {
            using (var identity = WindowsIdentity.GetCurrent())
                return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
        }

        /// False when cs2 runs at a higher integrity level than us, which happens whenever Steam was started as
        /// administrator: Windows then refuses every handle beyond basic info, so nothing below would work.
        public static bool CanAccess(Process process)
        {
            var handle = OpenProcess(0x0400 | 0x0010 | 0x00100000, false, process.Id); // query, vm read, synchronize
            if (handle == IntPtr.Zero)
                return Marshal.GetLastWin32Error() != 5;
            CloseHandle(handle);
            return true;
        }

        /// Starts this loader again as administrator, telling it to inject straight away. False if the UAC
        /// prompt was declined.
        public static bool RelaunchElevated()
        {
            try
            {
                Process.Start(new ProcessStartInfo(Application.ExecutablePath, "--inject") { UseShellExecute = true, Verb = "runas" });
                return true;
            }
            catch (Win32Exception e) when (e.NativeErrorCode == 1223)
            {
                return false;
            }
        }

        public static async Task<Process> WaitForProcessAsync(TimeSpan timeout)
        {
            var deadline = DateTime.UtcNow + timeout;
            while (DateTime.UtcNow < deadline)
            {
                var p = Find();
                if (p != null)
                    return p;
                await Task.Delay(1000);
            }
            return null;
        }

        /// True once the main window exists and every required module is loaded.
        public static async Task<bool> WaitUntilLoadedAsync(Process process, TimeSpan timeout)
        {
            var deadline = DateTime.UtcNow + timeout;
            while (DateTime.UtcNow < deadline)
            {
                if (process.HasExited)
                    return false;

                process.Refresh();
                if (process.MainWindowHandle != IntPtr.Zero)
                {
                    var names = ModuleNames(process);
                    if (RequiredModules.All(names.Contains))
                        return true;
                }
                await Task.Delay(1000);
            }
            return false;
        }

        public static void Inject(Process process, string dllPath)
        {
            const uint access = 0x0002 | 0x0008 | 0x0010 | 0x0020 | 0x0400; // create thread, vm op/read/write, query
            var handle = OpenProcess(access, false, process.Id);
            if (handle == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "couldn't open cs2");

            var remote = IntPtr.Zero;
            try
            {
                var path = Encoding.Unicode.GetBytes(dllPath + "\0");
                remote = VirtualAllocEx(handle, IntPtr.Zero, (UIntPtr)path.Length, 0x3000, 0x04);
                if (remote == IntPtr.Zero)
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "couldn't allocate in cs2");

                if (!WriteProcessMemory(handle, remote, path, (UIntPtr)path.Length, out _))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "couldn't write to cs2");

                // kernel32 sits at the same address in every process of a boot session.
                var load_library = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
                var thread = CreateRemoteThread(handle, IntPtr.Zero, UIntPtr.Zero, load_library, remote, 0, out _);
                if (thread == IntPtr.Zero)
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "couldn't start the load thread");

                try
                {
                    if (WaitForSingleObject(thread, 20000) != 0)
                        throw new TimeoutException("cs2 took too long to load the dll");

                    // LoadLibraryW's HMODULE, truncated to 32 bits; zero means it failed.
                    if (!GetExitCodeThread(thread, out var code) || code == 0)
                        throw new Exception("cs2 refused the dll (antivirus may have removed it)");
                }
                finally
                {
                    CloseHandle(thread);
                }
            }
            finally
            {
                if (remote != IntPtr.Zero)
                    VirtualFreeEx(handle, remote, UIntPtr.Zero, 0x8000);
                CloseHandle(handle);
            }
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint type, uint protect);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint type);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, UIntPtr size, out UIntPtr written);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandle(string name);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateRemoteThread(IntPtr process, IntPtr attributes, UIntPtr stack, IntPtr start, IntPtr param, uint flags, out uint id);

        [DllImport("kernel32.dll")]
        private static extern uint WaitForSingleObject(IntPtr handle, uint ms);

        [DllImport("kernel32.dll")]
        private static extern bool GetExitCodeThread(IntPtr thread, out uint code);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);
    }
}
