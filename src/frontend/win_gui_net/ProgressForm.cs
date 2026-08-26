/*
 * ProgressForm.cs -- modal progress dialog for BoltPack WinForms GUI
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

using System;
using System.Drawing;
using System.Windows.Forms;

namespace BoltPack
{
    public class ProgressForm : Form
    {
        private ProgressBar progressBar;
        private Label fileLabel;
        private Label countLabel;
        private Button cancelButton;

        public bool Cancelled { get; private set; }

        public ProgressForm(string title)
        {
            Text = title;
            Size = new Size(400, 160);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MaximizeBox = false;
            MinimizeBox = false;
            ControlBox = false;

            fileLabel = new Label
            {
                Location = new Point(12, 12),
                Size = new Size(370, 20),
                AutoEllipsis = true
            };

            progressBar = new ProgressBar
            {
                Location = new Point(12, 36),
                Size = new Size(370, 22),
                Minimum = 0,
                Maximum = 100
            };

            countLabel = new Label
            {
                Location = new Point(12, 64),
                Size = new Size(370, 20),
                ForeColor = Color.Gray
            };

            cancelButton = new Button
            {
                Text = "Cancel",
                Size = new Size(80, 28),
                Location = new Point(302, 88)
            };
            cancelButton.Click += delegate { Cancelled = true; };

            Controls.Add(fileLabel);
            Controls.Add(progressBar);
            Controls.Add(countLabel);
            Controls.Add(cancelButton);
        }

        public void UpdateProgress(string fileName, int percent, int current, int total)
        {
            UpdateProgress(fileName, percent,
                string.Format("File {0} of {1}", current, total));
        }

        public void UpdateProgress(string fileName, int percent, string statusText)
        {
            if (InvokeRequired)
            {
                Invoke(new Action<string, int, string>(UpdateProgress),
                       fileName, percent, statusText);
                return;
            }
            fileLabel.Text = fileName ?? "";
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            progressBar.Value = percent;
            countLabel.Text = statusText ?? "";
        }
    }
}
