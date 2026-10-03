using System;
using System.Net;
using System.Windows.Forms;

namespace aimwhere
{
    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            // .NET Framework defaults to TLS 1.0, which GitHub refuses.
            ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new LoaderForm());
        }
    }
}
