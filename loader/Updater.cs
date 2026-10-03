using System;
using System.IO;
using System.Net.Http;
using System.Threading.Tasks;

namespace aimwhere
{
    /// Keeps %LOCALAPPDATA%\aimwhere\aimwhere.dll in step with the public release repo. The repo holds only
    /// the DLL and a VERSION file; a VERSION that differs from the cached one means a new build to fetch.
    internal sealed class Updater
    {
        private const string BaseUrl = "https://raw.githubusercontent.com/biggestthighs/aimwhere/main/";

        public static readonly string Folder =
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "aimwhere");

        public static readonly string DllPath = Path.Combine(Folder, "aimwhere.dll");
        private static readonly string VersionPath = Path.Combine(Folder, "version.txt");

        public sealed class Result
        {
            public string Version;
            public string Note;
        }

        public static string CachedVersion =>
            File.Exists(VersionPath) && File.Exists(DllPath) ? File.ReadAllText(VersionPath).Trim() : null;

        public async Task<Result> EnsureLatestAsync(Action<string> status)
        {
            Directory.CreateDirectory(Folder);
            var cached = CachedVersion;

            string remote;
            using (var http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) })
            {
                http.DefaultRequestHeaders.UserAgent.ParseAdd("aimwhere-loader/1.0");
                try
                {
                    // The query string only dodges a stale cache in between; raw.githubusercontent ignores it.
                    remote = (await http.GetStringAsync(BaseUrl + "VERSION?t=" + DateTime.UtcNow.Ticks)).Trim();
                }
                catch (Exception) when (cached != null)
                {
                    return new Result { Version = cached, Note = "offline, using cached build" };
                }

                if (remote == cached)
                    return new Result { Version = cached, Note = "up to date" };

                status("downloading v" + remote + "...");
                var bytes = await http.GetByteArrayAsync(BaseUrl + "aimwhere.dll?t=" + DateTime.UtcNow.Ticks);

                // A truncated download or an HTML error page must never replace a working build.
                if (bytes.Length < 1024 * 1024 || bytes[0] != (byte)'M' || bytes[1] != (byte)'Z')
                {
                    if (cached != null)
                        return new Result { Version = cached, Note = "download was corrupt, using cached build" };
                    throw new InvalidDataException("the downloaded dll is corrupt, try again");
                }

                var temp = DllPath + ".tmp";
                File.WriteAllBytes(temp, bytes);
                try
                {
                    if (File.Exists(DllPath))
                        File.Delete(DllPath);
                    File.Move(temp, DllPath);
                }
                catch (IOException) when (cached != null)
                {
                    // CS2 still has the old build loaded and locked.
                    File.Delete(temp);
                    return new Result { Version = cached, Note = "close cs2 to install v" + remote };
                }
                catch (UnauthorizedAccessException) when (cached != null)
                {
                    File.Delete(temp);
                    return new Result { Version = cached, Note = "close cs2 to install v" + remote };
                }

                File.WriteAllText(VersionPath, remote);
                return new Result { Version = remote, Note = cached == null ? "installed" : "updated" };
            }
        }
    }
}
