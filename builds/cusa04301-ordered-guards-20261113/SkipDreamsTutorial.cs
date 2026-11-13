using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using System.Windows.Forms;

// Optional GUI equivalent of the existing tutorial helper. No bundled save data.
internal static class SkipDreamsTutorial
{
    internal static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
    internal static string Patch(string text, out int changed)
    {
        var json = new JavaScriptSerializer { MaxJsonLength = Int32.MaxValue };
        var root = json.Deserialize<Dictionary<string, object>>(text);
        var progress = root["player_progress"] as Dictionary<string, object>;
        if (progress == null) throw new InvalidDataException("Unsupported Dreams save format.");
        changed = 0;
        foreach (var entry in new Dictionary<string, int> {
            {"played_tutorial_vo", 158}, {"played_tutorial_cutscene", 6}, {"progress_unlocked", 56}})
        {
            object value;
            if (!progress.TryGetValue(entry.Key, out value)) throw new InvalidDataException("Missing " + entry.Key);
            var flags = value as IList;
            if (flags == null || flags.Count != entry.Value || flags.Cast<object>().Any(v => !(v is bool)))
                throw new InvalidDataException("Unsupported " + entry.Key + " array.");
            var matches = Regex.Matches(text, "\"" + entry.Key + "\"\\s*:\\s*\\[(?<flags>\\s*(?:true|false)(?:\\s*,\\s*(?:true|false))*\\s*)\\]");
            if (matches.Count != 1) throw new InvalidDataException("Ambiguous " + entry.Key + " array.");
            var body = matches[0].Groups["flags"];
            var tokens = Regex.Matches(body.Value, "true|false");
            if (tokens.Count != entry.Value) throw new InvalidDataException("Unexpected flag count.");
            var result = new StringBuilder(); int cursor = 0;
            for (int i = 0; i < tokens.Count; i++)
            {
                var token = tokens[i];
                result.Append(body.Value.Substring(cursor, token.Index - cursor));
                bool complete = entry.Key != "played_tutorial_vo" || i < 85 || i > 87;
                if (complete && token.Value == "false") { result.Append("true"); changed++; }
                else result.Append(token.Value);
                cursor = token.Index + token.Length;
            }
            result.Append(body.Value.Substring(cursor));
            text = text.Substring(0, body.Index) + result + text.Substring(body.Index + body.Length);
        }
        json.DeserializeObject(text);
        return text;
    }

    internal static void CheckClosed()
    {
        var processes = Process.GetProcessesByName("shadps4");
        try { if (processes.Length != 0) throw new InvalidOperationException("Close Dreams normally first. No save was changed."); }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    internal static void CheckPath(string path)
    {
        var file = new FileInfo(path);
        if (!file.Exists || !file.Name.Equals("savedata.json", StringComparison.OrdinalIgnoreCase) ||
            !file.Directory.Name.Equals("SYSTEM", StringComparison.OrdinalIgnoreCase) ||
            !file.Directory.Parent.Name.Equals("CUSA04301", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Select CUSA04301\\SYSTEM\\savedata.json from your own profile.");
        for (FileSystemInfo p = file; p != null; p = p is FileInfo ? ((FileInfo)p).Directory : ((DirectoryInfo)p).Parent)
            if ((p.Attributes & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Linked save paths are not supported.");
    }

    internal static byte[] Updated(byte[] original, out int changes)
    {
        int bom = original.Length >= 3 && original[0] == 239 && original[1] == 187 && original[2] == 191 ? 3 : 0;
        byte[] body = Utf8.GetBytes(Patch(Utf8.GetString(original, bom, original.Length - bom), out changes));
        return bom == 0 ? body : new byte[] {239, 187, 191}.Concat(body).ToArray();
    }

    internal static string ReplaceWithBackup(string path, byte[] original, byte[] updated)
    {
        string id = DateTime.UtcNow.ToString("yyyyMMddTHHmmssfffZ") + "-" + Guid.NewGuid().ToString("N");
        string temporary = path + ".tutorial-pending-" + id, backup = path + ".before-tutorial-skip-" + id + ".bak";
        try
        {
            File.WriteAllBytes(temporary, updated);
            if (!File.ReadAllBytes(path).SequenceEqual(original)) throw new IOException("The save changed during preparation. Nothing was replaced.");
            File.Replace(temporary, path, backup);
            return backup;
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    [STAThread]
    private static void Main()
    {
        Application.EnableVisualStyles();
        try
        {
            CheckClosed();
            using (var picker = new OpenFileDialog { Title = "Select your Dreams SYSTEM save", Filter = "Dreams savedata.json|savedata.json", CheckFileExists = true })
            {
                picker.InitialDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "shadPS4");
                if (picker.ShowDialog() != DialogResult.OK) return;
                CheckPath(picker.FileName);
                byte[] original = File.ReadAllBytes(picker.FileName); int changes;
                byte[] updated = Updated(original, out changes);
                if (changes == 0) { MessageBox.Show("Tutorial progression is already unlocked. No save changed.", "Dreams tutorial"); return; }
                if (MessageBox.Show("Unlock tutorial progression in your selected save? An exact backup will be created; creations and other settings are preserved.", "Dreams tutorial", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
                CheckClosed(); CheckPath(picker.FileName);
                string backup = ReplaceWithBackup(picker.FileName, original, updated);
                MessageBox.Show("Tutorial progression unlocked. Start Dreams again.\n\nBackup: " + backup, "Dreams tutorial");
            }
        }
        catch (Exception e) { MessageBox.Show(e.Message, "Dreams tutorial", MessageBoxButtons.OK, MessageBoxIcon.Error); }
    }
}
