/*
 * DocForm.cs -- Documentation viewer for BoltPack WinForms GUI
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

using System;
using System.Drawing;
using System.Windows.Forms;

namespace BoltPack
{
    public class DocForm : Form
    {
        public DocForm()
        {
            Text = "BoltPack Documentation";
            Size = new Size(520, 440);
            StartPosition = FormStartPosition.CenterParent;

            var textBox = new TextBox
            {
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Vertical,
                Dock = DockStyle.Fill,
                Font = new Font("Consolas", 9.5f),
                Text = HelpText
            };

            var okButton = new Button
            {
                Text = "Close",
                Size = new Size(80, 28),
                Dock = DockStyle.Bottom,
                DialogResult = DialogResult.OK
            };

            Controls.Add(textBox);
            Controls.Add(okButton);
            AcceptButton = okButton;
        }

        private const string HelpText =
            "BoltPack - Quick Guide\r\n" +
            "======================\r\n" +
            "\r\n" +
            "BoltPack is a lightweight file archiver designed for maximum\r\n" +
            "portability across vintage and modern systems.\r\n" +
            "\r\n" +
            "Opening an Archive\r\n" +
            "-------------------\r\n" +
            "Use Archive > Open to select a .bolt or .blt file.\r\n" +
            "The main window will display the archive contents\r\n" +
            "including file names, sizes, compression ratios,\r\n" +
            "and timestamps.\r\n" +
            "\r\n" +
            "You can also drag and drop a .bolt file onto the\r\n" +
            "main window to open it.\r\n" +
            "\r\n" +
            "Extracting Files\r\n" +
            "-----------------\r\n" +
            "With an archive open, use Archive > Extract Files.\r\n" +
            "Select a destination folder and the files will be\r\n" +
            "extracted. A progress window shows the current file\r\n" +
            "and overall progress.\r\n" +
            "\r\n" +
            "Creating an Archive\r\n" +
            "--------------------\r\n" +
            "Use Archive > Create New to select files and folders\r\n" +
            "to archive. Choose a destination and file name for\r\n" +
            "the new archive. A progress window shows the packing\r\n" +
            "progress.\r\n" +
            "\r\n" +
            "You can also drag and drop files or folders onto the\r\n" +
            "main window to create a new archive from them.\r\n" +
            "\r\n" +
            "Drag and Drop\r\n" +
            "--------------\r\n" +
            "Drop .bolt/.blt files to open them.\r\n" +
            "Drop other files or folders to create a new archive.\r\n" +
            "\r\n" +
            "Compression Methods\r\n" +
            "--------------------\r\n" +
            "BoltPack supports four compression methods:\r\n" +
            "  Stored  - No compression\r\n" +
            "  LZSS    - Small-memory LZ77 for 16-bit systems\r\n" +
            "  boltlz - Fast general-purpose (default)\r\n" +
            "  fluff   - Best compression ratio\r\n" +
            "\r\n" +
            "Keyboard Shortcuts\r\n" +
            "-------------------\r\n" +
            "  Ctrl+O  - Open archive\r\n" +
            "  Ctrl+N  - Create new archive\r\n" +
            "  Ctrl+Q  - Quit\r\n";
    }
}
