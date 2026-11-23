using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;

class PokkeLab {
    [STAThread]
    static void Main() {
        string dir = Path.Combine(Path.GetTempPath(), "PokkeLab", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        foreach (string file in new[] { "index.html", "compare.js" }) {
            using (Stream source = Assembly.GetExecutingAssembly().GetManifestResourceStream(file))
            using (FileStream target = File.Create(Path.Combine(dir, file))) source.CopyTo(target);
        }
        Process.Start(new ProcessStartInfo(Path.Combine(dir, "index.html")) { UseShellExecute = true });
    }
}