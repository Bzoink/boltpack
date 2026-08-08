/*
 * AboutForm.cs -- About dialog for BoltPack WinForms GUI
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

using System;
using System.Drawing;
using System.Windows.Forms;

namespace BoltPack
{
    public class AboutForm : Form
    {
        public AboutForm()
        {
            Text = "About BoltPack";
            Size = new Size(340, 200);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MaximizeBox = false;
            MinimizeBox = false;

            var titleLabel = new Label
            {
                Text = "BoltPack 1.1",
                Font = new Font("Segoe UI", 14, FontStyle.Bold),
                Location = new Point(20, 16),
                AutoSize = true
            };

            var descLabel = new Label
            {
                Text = "A lightweight file archiver for\nvintage and modern systems",
                Location = new Point(20, 50),
                Size = new Size(290, 36)
            };

            var copyrightLabel = new Label
            {
                Text = "Copyright \u00A9 2026 Tony Mattsson\n"
                     + "Released under the GNU General Public License v2",
                Location = new Point(20, 92),
                Size = new Size(290, 36),
                ForeColor = Color.Gray
            };

            var okButton = new Button
            {
                Text = "OK",
                Size = new Size(80, 28),
                Location = new Point(230, 135),
                DialogResult = DialogResult.OK
            };

            Controls.Add(titleLabel);
            Controls.Add(descLabel);
            Controls.Add(copyrightLabel);
            Controls.Add(okButton);
            AcceptButton = okButton;
        }
    }
}
