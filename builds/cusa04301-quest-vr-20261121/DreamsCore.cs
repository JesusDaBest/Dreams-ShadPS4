using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

// Launch adapter only. The tested emulator executable is copied without modification.
internal static class DreamsLauncher
{
    private static readonly JavaScriptSerializer Json = new JavaScriptSerializer();
    private static readonly string Root = AppDomain.CurrentDomain.BaseDirectory;
    private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);
    private const string QtForce60Hz = "SHADPS4_QT_DREAMS_FORCE_60HZ_RENDERING";

    public sealed class Settings
    {
        public string Core { get; set; }
        public string CoreSha256 { get; set; }
        public string ConfigSha256 { get; set; }
        public string EnvironmentSha256 { get; set; }
        public string WorkingDirectory { get; set; }
        public string ProfileRoot { get; set; }
        public string DreamsGame { get; set; }
        public string FallbackCore { get; set; }
        public string[] ExistingDreamsCores { get; set; }
    }

    private static string Hash(string path)
    {
        using (var sha = SHA256.Create())
        using (var input = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(input)).Replace("-", "").ToLowerInvariant();
    }

    private static void CheckHash(string path, string expected)
    {
        if (!String.Equals(Hash(path), expected, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The verified Dreams package has changed: " + path);
    }

    private static bool SamePath(string left, string right)
    {
        return String.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);
    }

    private static string GamePath(List<string> args)
    {
        for (int i = 0; i < args.Count; i++)
        {
            if (args[i] == "--") break;
            if ((args[i] == "--game" || args[i] == "-g") && i + 1 < args.Count) return args[i + 1];
            if (args[i].StartsWith("--game=", StringComparison.Ordinal)) return args[i].Substring(7);
        }
        return null;
    }

    private static bool IsDreams(string game, Settings settings)
    {
        if (String.IsNullOrEmpty(game)) return false;
        if (SamePath(game, settings.DreamsGame)) return true;
        string sfo = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(game)), "sce_sys", "param.sfo");
        if (!File.Exists(sfo)) return false;
        byte[] bytes = File.ReadAllBytes(sfo);
        if (bytes.Length < 20 || BitConverter.ToUInt32(bytes, 0) != 0x46535000) return false;
        uint keys = BitConverter.ToUInt32(bytes, 8), values = BitConverter.ToUInt32(bytes, 12);
        uint count = BitConverter.ToUInt32(bytes, 16);
        if (count > (bytes.Length - 20) / 16) return false;
        for (int i = 0; i < count; i++)
        {
            int entry = 20 + i * 16;
            long key = (long)keys + BitConverter.ToUInt16(bytes, entry);
            long value = (long)values + BitConverter.ToUInt32(bytes, entry + 12);
            uint size = BitConverter.ToUInt32(bytes, entry + 4);
            if (key < 0 || key >= bytes.Length || value < 0 || value + size > bytes.Length) continue;
            int end = Array.IndexOf(bytes, (byte)0, (int)key);
            if (end < 0) continue;
            if (Encoding.UTF8.GetString(bytes, (int)key, end - (int)key) == "TITLE_ID")
                return Encoding.UTF8.GetString(bytes, (int)value, (int)size).TrimEnd('\0') == "CUSA04301";
        }
        return false;
    }

    // Windows CommandLineToArgvW quoting, including trailing backslashes and embedded quotes.
    private static string Quote(string arg)
    {
        var result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in arg)
        {
            if (c == '\\') { slashes++; continue; }
            result.Append('\\', c == '"' ? slashes * 2 + 1 : slashes);
            result.Append(c);
            slashes = 0;
        }
        result.Append('\\', slashes * 2);
        return result.Append('"').ToString();
    }

    private static void WriteJson(string path, object value)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
        File.WriteAllText(path, Json.Serialize(value), Utf8);
    }

    private static void ApplyConfig(string source, string destination, string expected)
    {
        if (File.Exists(destination) && Hash(destination) == expected) return;
        Directory.CreateDirectory(Path.GetDirectoryName(destination));
        if (File.Exists(destination))
        {
            string backupDir = Path.Combine(Root, "settings-backups");
            Directory.CreateDirectory(backupDir);
            string backup = Path.Combine(backupDir, "CUSA04301-" + Hash(destination) + ".json");
            if (!File.Exists(backup)) File.Copy(destination, backup);
        }
        string temporary = destination + ".dreams-tested-" + Guid.NewGuid().ToString("N") + ".tmp";
        File.Copy(source, temporary);
        try
        {
            if (File.Exists(destination)) File.Replace(temporary, destination, null);
            else File.Move(temporary, destination);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private static void CheckNoExistingDreams(Settings settings, string core)
    {
        var known = new List<string>(settings.ExistingDreamsCores ?? new string[0]);
        known.Add(core);
        foreach (Process process in Process.GetProcessesByName("shadps4"))
        using (process)
        {
            string path;
            try { path = process.MainModule.FileName; }
            catch (System.ComponentModel.Win32Exception) { continue; }
            catch (InvalidOperationException) { continue; }
            if (known.Any(candidate => SamePath(candidate, path)))
                throw new InvalidOperationException("Dreams is already open. Close its game window normally before launching it again. Your running game has been left open.");
        }
    }

    private static string SteamVrLibrary()
    {
        string registry = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                       "openvr", "openvrpaths.vrpath");
        if (File.Exists(registry))
        {
            var paths = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(registry));
            object value;
            if (paths != null && paths.TryGetValue("runtime", out value) && value is IList)
            {
                foreach (var root in ((IList)value).OfType<string>())
                {
                    if (!Path.IsPathRooted(root)) continue;
                    string dll = Path.GetFullPath(Path.Combine(root, "bin", "win64", "openvr_api.dll"));
                    if (File.Exists(dll)) return dll;
                }
            }
        }
        throw new InvalidOperationException("Start SteamVR and connect your headset, then launch Dreams again.");
    }

    [STAThread]
    private static int Main(string[] arguments)
    {
        bool verify = false;
        try
        {
            var args = new List<string>(arguments);
            string reportPath = null;
            int verifyIndex = args.IndexOf("--dreams-verify");
            if (verifyIndex >= 0)
            {
                verify = true;
                if (verifyIndex + 1 >= args.Count) throw new ArgumentException("Missing verification output path.");
                reportPath = args[verifyIndex + 1];
                args.RemoveRange(verifyIndex, 2);
            }
            var settings = new Settings {
                Core = "core/shadps4.exe",
                CoreSha256 = "57caf8694abdb800f69dc729bd79a2ff49e53732a79771d10e6d7ce88f43c397",
                ConfigSha256 = "56a098abbc3610498214904ff4e87591ae6b82c2e3ef9b198581ec24c74c8d04",
                EnvironmentSha256 = "6d041b0b879481accf500c728a9b7d02cc4afa84898995effcdd1b9087266c58",
                WorkingDirectory = Root,
                ProfileRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "shadPS4"),
                DreamsGame = Path.Combine(Root, "no-bundled-game", "eboot.bin"),
                FallbackCore = Path.Combine(Root, "core", "shadps4.exe"),
                ExistingDreamsCores = new string[0]
            };
            bool dreams = IsDreams(GamePath(args), settings);
            string core = dreams ? Path.GetFullPath(Path.Combine(Root, settings.Core)) : settings.FallbackCore;
            string cwd = dreams ? settings.WorkingDirectory : Environment.CurrentDirectory;
            var start = new ProcessStartInfo(core) {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = cwd,
                RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true
            };
            bool explicitVr = args.Remove("--dreams-vr");
            bool useVr = dreams && (explicitVr || start.EnvironmentVariables["SHADPS4_QT_DREAMS_OPENVR"] == "1");
            start.EnvironmentVariables.Remove("SHADPS4_QT_DREAMS_OPENVR");
            string qtForce60Hz = start.EnvironmentVariables[QtForce60Hz];
            start.EnvironmentVariables.Remove(QtForce60Hz);
            string config = Path.Combine(Root, "CUSA04301.json");
            string target = Path.Combine(settings.ProfileRoot, "custom_configs", "CUSA04301.json");
            string queue = null;
            if (dreams)
            {
                CheckHash(core, settings.CoreSha256);
                CheckHash(config, settings.ConfigSha256);
                string environment = Path.Combine(Root, "environment.json");
                CheckHash(environment, settings.EnvironmentSha256);
                if (Directory.Exists(Path.Combine(cwd, "user")))
                    throw new InvalidOperationException("A portable user folder would hide your normal Dreams saves: " + Path.Combine(cwd, "user"));
                if (args.Any(arg => arg == "--config-clean" || arg == "--config-global" || arg.StartsWith("--override-root", StringComparison.Ordinal)))
                    throw new InvalidOperationException("This launch option bypasses the verified Dreams profile.");
                // Normal Play adds no display override; match the tested windowed command line.
                if (!args.Any(arg => arg == "--fullscreen" || arg == "-f" || arg.StartsWith("--fullscreen=", StringComparison.Ordinal)))
                    args.InsertRange(0, new string[] { "--fullscreen", "false" });
                foreach (string key in start.EnvironmentVariables.Keys.Cast<string>().ToArray())
                    if (key.StartsWith("SHADPS4_", StringComparison.OrdinalIgnoreCase)) start.EnvironmentVariables.Remove(key);
                var env = Json.Deserialize<Dictionary<string, string>>(File.ReadAllText(environment));
                queue = Path.Combine(Root, "queue-state", verify ? "verification-only" : Guid.NewGuid().ToString("N"));
                foreach (var pair in env)
                    start.EnvironmentVariables[pair.Key] = pair.Key == "SHADPS4_DREAMS_PRIMARY_QUEUE_PROBE_DIR" ? queue : pair.Key == "SHADPS4_DREAMS_PRIVATE_SHADER_CACHE" ? Path.Combine(Root, "shader-cache") : pair.Value;
                if (qtForce60Hz == "0" || qtForce60Hz == "1")
                    start.EnvironmentVariables["SHADPS4_DREAMS_FORCE_60HZ_RENDERING"] = qtForce60Hz;
                if (useVr)
                {
                    string game = Path.GetFullPath(GamePath(args));
                    if (Directory.Exists(game)) game = Path.Combine(game, "eboot.bin");
                    CheckHash(game, "f3d3711120278471c4c7fa84aa6bd321700d9adbea0d6e53be9665a4bcb6383f");
                    string patch = Path.Combine(Path.GetDirectoryName(core), "quest-vr.xml");
                    CheckHash(patch, "17034f54ffdbfe86be68752056630fecb37bf0e58e1cf437cbf6e189726a2d3a");
                    start.EnvironmentVariables["SHADPS4_DREAMS_OPENVR"] = "1";
                    start.EnvironmentVariables["SHADPS4_DREAMS_OPENVR_SEATED_PAD"] = "1";
                    start.EnvironmentVariables["SHADPS4_DREAMS_OPENVR_PHONE_PAD"] = "0";
                    start.EnvironmentVariables["SHADPS4_DREAMS_QUEST_HANDS"] = "1";
                    start.EnvironmentVariables["SHADPS4_DREAMS_FORCE_60HZ_RENDERING"] = "1";
                    start.EnvironmentVariables["SHADPS4_OPENVR_API_PATH"] = SteamVrLibrary();
                    args.InsertRange(0, new string[] { "-p", patch });
                }
            }
            start.Arguments = String.Join(" ", args.Select(Quote));
            var childEnv = new SortedDictionary<string, string>();
            foreach (string key in start.EnvironmentVariables.Keys)
                if (key.StartsWith("SHADPS4_", StringComparison.OrdinalIgnoreCase)) childEnv[key] = start.EnvironmentVariables[key];
            var report = new Dictionary<string, object> {
                {"dreams", dreams}, {"core", core}, {"core_sha256", Hash(core)},
                {"arguments", args}, {"working_directory", cwd}, {"profile", settings.ProfileRoot},
                {"configuration", target}, {"configuration_sha256", dreams ? Hash(config) : null},
                {"configuration_already_matches", dreams && File.Exists(target) && Hash(target) == settings.ConfigSha256},
                {"environment", childEnv}, {"verification_only", verify}, {"utc", DateTime.UtcNow.ToString("o")}
            };
            if (verify)
            {
                WriteJson(reportPath, report);
                return 0;
            }
            if (dreams)
            {
                CheckNoExistingDreams(settings, core);
                ApplyConfig(config, target, settings.ConfigSha256);
                Directory.CreateDirectory(queue);
                File.WriteAllText(Path.Combine(queue, "mode.txt"), "native\n", Utf8);
            }
            using (var child = new Process { StartInfo = start })
            {
                child.Start();
                // Keep Qt's process lifetime and output pipes attached to the actual core.
                var stdout = child.StandardOutput.BaseStream.CopyToAsync(Console.OpenStandardOutput());
                var stderr = child.StandardError.BaseStream.CopyToAsync(Console.OpenStandardError());
                if (dreams) child.StandardInput.Close(); // Exactly the tested non-IPC launch.
                var stdin = new Thread(() => {
                    try
                    {
                        Stream input = Console.OpenStandardInput();
                        if (dreams) input.CopyTo(Stream.Null);
                        else { input.CopyTo(child.StandardInput.BaseStream); child.StandardInput.Close(); }
                    }
                    catch (IOException) { }
                    catch (InvalidOperationException) { }
                });
                stdin.IsBackground = true;
                stdin.Start();
                if (dreams)
                {
                    report["pid"] = child.Id;
                    report["status"] = "running";
                    WriteJson(Path.Combine(Root, "last-launch.json"), report);
                }
                child.WaitForExit();
                System.Threading.Tasks.Task.WaitAll(stdout, stderr);
                if (dreams)
                {
                    report["status"] = "exited";
                    report["exit_code"] = child.ExitCode;
                    WriteJson(Path.Combine(Root, "last-launch.json"), report);
                }
                return child.ExitCode;
            }
        }
        catch (Exception error)
        {
            try { WriteJson(Path.Combine(Root, "last-error.json"), new { utc = DateTime.UtcNow.ToString("o"), error = error.ToString() }); } catch { }
            Console.Error.WriteLine(error.Message);
            if (!verify) MessageBox.Show(error.Message, "Dreams launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
