using System;
using System.Net;
using System.Windows.Forms;

namespace aimwhere
{
    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
        {
            // .NET Framework defaults to TLS 1.0, which GitHub refuses.
            ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            // --inject: relaunched as administrator because cs2 was, so carry on with the inject the user clicked.
            Application.Run(new LoaderForm(Array.IndexOf(args, "--inject") >= 0));
        }
    }
}
