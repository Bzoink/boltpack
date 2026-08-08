/*
 * Program.cs -- entry point for BoltPack WinForms GUI
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

using System;
using System.Windows.Forms;

namespace BoltPack
{
    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            var form = new MainForm();

            /* If launched with a file argument, open it */
            if (args.Length > 0 && !string.IsNullOrEmpty(args[0]))
                form.OpenArchiveOnLoad = args[0];

            Application.Run(form);
        }
    }
}
