/*
 * MainForm.cs -- main window for BoltPack WinForms GUI
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace BoltPack
{
    public class MainForm : Form
    {
        /* --- Controls --- */
        private MenuStrip menuStrip;
        private ListView listView;
        private StatusStrip statusStrip;
        private ToolStripStatusLabel statusLabel;
        private Label welcomeLabel;

        /* --- State --- */
        private string archivePath;
        private IntPtr archiveFp;
        private BpArchiveInfo archiveInfo;
        private bool initOk;

        /* --- Public property for command-line open --- */
        public string OpenArchiveOnLoad { get; set; }

        /* ================================================================ */
        /*  Construction                                                    */
        /* ================================================================ */

        public MainForm()
        {
            InitializeComponent();
            initOk = (NativeMethods.bp_dll_compress_init() == NativeMethods.BP_OK);
        }

        private void InitializeComponent()
        {
            Text = "BoltPack";
            Size = new Size(720, 480);
            StartPosition = FormStartPosition.CenterScreen;
            AllowDrop = true;

            /* Menu */
            menuStrip = new MenuStrip();

            var archiveMenu = new ToolStripMenuItem("&Archive");
            archiveMenu.DropDownItems.Add(MakeItem("&Open...", Keys.Control | Keys.O, OnOpen));
            archiveMenu.DropDownItems.Add(MakeItem("&Extract Files...", Keys.Control | Keys.E, OnExtract));
            archiveMenu.DropDownItems.Add(MakeItem("&Close", Keys.Control | Keys.W, OnClose));
            archiveMenu.DropDownItems.Add(MakeItem("&Validate", Keys.Control | Keys.T, OnValidate));
            archiveMenu.DropDownItems.Add(new ToolStripSeparator());
            archiveMenu.DropDownItems.Add(MakeItem("Create &New...", Keys.Control | Keys.N, OnCreate));
            archiveMenu.DropDownItems.Add(new ToolStripSeparator());
            archiveMenu.DropDownItems.Add(MakeItem("E&xit", Keys.Alt | Keys.F4, OnExit));
            menuStrip.Items.Add(archiveMenu);

            var helpMenu = new ToolStripMenuItem("&Help");
            helpMenu.DropDownItems.Add(MakeItem("&Documentation...", Keys.F1, OnDoc));
            helpMenu.DropDownItems.Add(MakeItem("&About BoltPack...", Keys.None, OnAbout));
            menuStrip.Items.Add(helpMenu);

            /* ListView */
            listView = new ListView
            {
                Dock = DockStyle.Fill,
                View = View.Details,
                FullRowSelect = true,
                GridLines = true,
                Font = new Font("Segoe UI", 9)
            };
            listView.Columns.Add("Name", 240);
            listView.Columns.Add("Original", 90, HorizontalAlignment.Right);
            listView.Columns.Add("Packed", 90, HorizontalAlignment.Right);
            listView.Columns.Add("Ratio", 55, HorizontalAlignment.Right);
            listView.Columns.Add("Method", 70);
            listView.Columns.Add("Date", 130);

            /* Welcome label (shown when no archive is open) */
            welcomeLabel = new Label
            {
                Text = "BoltPack\n\nDrop a .bolt file here to open it,\nor drop files to create an archive.",
                TextAlign = ContentAlignment.MiddleCenter,
                Dock = DockStyle.Fill,
                Font = new Font("Segoe UI", 11),
                ForeColor = Color.Gray
            };

            /* Status bar */
            statusStrip = new StatusStrip();
            statusLabel = new ToolStripStatusLabel("Ready");
            statusStrip.Items.Add(statusLabel);

            /* Layout */
            Controls.Add(listView);
            Controls.Add(welcomeLabel);
            Controls.Add(statusStrip);
            Controls.Add(menuStrip);
            MainMenuStrip = menuStrip;

            listView.Visible = false;
            welcomeLabel.Visible = true;

            /* Events */
            DragEnter += MainForm_DragEnter;
            DragDrop += MainForm_DragDrop;
            Shown += MainForm_Shown;
            FormClosed += MainForm_FormClosed;
        }

        private static ToolStripMenuItem MakeItem(string text, Keys shortcut, EventHandler handler)
        {
            var item = new ToolStripMenuItem(text);
            if (shortcut != Keys.None) item.ShortcutKeys = shortcut;
            item.Click += handler;
            return item;
        }

        /* ================================================================ */
        /*  Event handlers                                                  */
        /* ================================================================ */

        private void MainForm_Shown(object sender, EventArgs e)
        {
            if (!initOk)
            {
                MessageBox.Show(this, "Failed to initialize compression engine.",
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (!string.IsNullOrEmpty(OpenArchiveOnLoad))
                DoOpen(OpenArchiveOnLoad);
        }

        private void MainForm_FormClosed(object sender, FormClosedEventArgs e)
        {
            CloseArchive();
        }

        /* --- Menu handlers --- */

        private void OnOpen(object sender, EventArgs e)
        {
            using (var dlg = new OpenFileDialog())
            {
                dlg.Filter = "BoltPack archives (*.bolt;*.blt)|*.bolt;*.blt|All files (*.*)|*.*";
                if (dlg.ShowDialog(this) == DialogResult.OK)
                    DoOpen(dlg.FileName);
            }
        }

        private void OnExtract(object sender, EventArgs e)
        {
            if (archivePath == null)
            {
                MessageBox.Show(this, "No archive is open.", "BoltPack",
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            using (var dlg = new FolderBrowserDialog())
            {
                dlg.Description = "Select destination folder";
                if (dlg.ShowDialog(this) == DialogResult.OK)
                    DoExtract(dlg.SelectedPath);
            }
        }

        private void OnClose(object sender, EventArgs e)
        {
            CloseArchive();
            listView.Items.Clear();
            listView.Visible = false;
            welcomeLabel.Visible = true;
            Text = "BoltPack";
            statusLabel.Text = "Ready";
        }

        private void OnValidate(object sender, EventArgs e)
        {
            if (archivePath == null)
            {
                MessageBox.Show(this, "No archive is open.", "BoltPack",
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            DoValidate();
        }

        private void OnCreate(object sender, EventArgs e)
        {
            using (var dlg = new OpenFileDialog())
            {
                dlg.Filter = "All files (*.*)|*.*";
                dlg.Multiselect = true;
                dlg.Title = "Select files to archive";
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                DoCreate(dlg.FileNames);
            }
        }

        private void OnExit(object sender, EventArgs e) { Close(); }

        private void OnDoc(object sender, EventArgs e)
        {
            var docForm = new DocForm();
            docForm.ShowDialog(this);
        }

        private void OnAbout(object sender, EventArgs e)
        {
            var aboutForm = new AboutForm();
            aboutForm.ShowDialog(this);
        }

        /* --- Drag and drop --- */

        private void MainForm_DragEnter(object sender, DragEventArgs e)
        {
            if (e.Data.GetDataPresent(DataFormats.FileDrop))
                e.Effect = DragDropEffects.Copy;
        }

        private void MainForm_DragDrop(object sender, DragEventArgs e)
        {
            var files = (string[])e.Data.GetData(DataFormats.FileDrop);
            if (files == null || files.Length == 0) return;
            ClassifyAndAct(files);
        }

        private void ClassifyAndAct(string[] paths)
        {
            /* If exactly one file with .bolt/.blt extension, open it */
            if (paths.Length == 1)
            {
                string ext = Path.GetExtension(paths[0]).ToLowerInvariant();
                if (ext == ".bolt" || ext == ".blt")
                {
                    DoOpen(paths[0]);
                    return;
                }
            }
            /* Otherwise create an archive from the dropped items */
            DoCreate(paths);
        }

        /* ================================================================ */
        /*  Archive operations                                              */
        /* ================================================================ */

        private void CloseArchive()
        {
            if (archiveFp != IntPtr.Zero)
            {
                NativeMethods.bp_dll_fclose(archiveFp);
                archiveFp = IntPtr.Zero;
            }
            archivePath = null;
            archiveInfo = new BpArchiveInfo();
        }

        private void DoOpen(string path)
        {
            CloseArchive();
            listView.Items.Clear();

            archiveFp = NativeMethods.bp_dll_fopen(path, "rb");
            if (archiveFp == IntPtr.Zero)
            {
                MessageBox.Show(this, "Cannot open file:\n" + path,
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            archivePath = path;

            /* Read archive info */
            BpIo io = new BpIo();
            NativeMethods.bp_dll_io_from_file(ref io, archiveFp);

            archiveInfo = new BpArchiveInfo();
            int rc = NativeMethods.bp_dll_archive_info(ref io, ref archiveInfo);
            if (rc != NativeMethods.BP_OK)
            {
                MessageBox.Show(this, "Not a valid BoltPack archive.",
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                CloseArchive();
                return;
            }

            /* Populate the file list */
            NativeMethods.bp_dll_fseek_start(archiveFp);
            NativeMethods.bp_dll_io_from_file(ref io, archiveFp);

            var entries = new List<BpEntry>();
            BpListDelegate listCb = delegate (ref BpEntry entry, IntPtr ud)
            {
                entries.Add(entry);
                return 0;
            };

            rc = NativeMethods.bp_dll_archive_list(ref io, listCb, IntPtr.Zero);
            if (rc != NativeMethods.BP_OK)
            {
                MessageBox.Show(this, "Error reading archive contents.",
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                CloseArchive();
                return;
            }

            listView.BeginUpdate();
            foreach (var ent in entries)
            {
                string name = ent.name != IntPtr.Zero
                    ? Marshal.PtrToStringAnsi(ent.name) : "(unknown)";
                double orig = ent.original_size.ToDouble();
                double comp = ent.compressed_size.ToDouble();
                string ratio = orig > 0
                    ? ((1.0 - comp / orig) * 100).ToString("0") + "%"
                    : "-";

                var item = new ListViewItem(name);
                item.SubItems.Add(FormatSize(orig));
                item.SubItems.Add(FormatSize(comp));
                item.SubItems.Add(ratio);
                item.SubItems.Add(NativeMethods.MethodName(ent.method));
                item.SubItems.Add(FormatTime(ent.mtime));

                if (ent.entry_type == NativeMethods.BP_TYPE_DIR)
                    item.ForeColor = Color.DarkBlue;

                listView.Items.Add(item);
            }
            listView.EndUpdate();

            welcomeLabel.Visible = false;
            listView.Visible = true;
            Text = "BoltPack - " + Path.GetFileName(path);
            statusLabel.Text = string.Format("{0} entries, {1} total",
                archiveInfo.entry_count, FormatSize(archiveInfo.total_original.ToDouble()));

            /* prevent the callback delegate from being collected */
            GC.KeepAlive(listCb);
        }

        /* ---------------------------------------------------------------- */
        /*  Extract                                                         */
        /* ---------------------------------------------------------------- */

        private void DoExtract(string destDir)
        {
            NativeMethods.bp_dll_fseek_start(archiveFp);

            BpIo io = new BpIo();
            NativeMethods.bp_dll_io_from_file(ref io, archiveFp);

            using (var prog = new ProgressForm("Extracting..."))
            {
                prog.Show(this);
                Enabled = false;

                var ctx = new ExtractContext { destDir = destDir, form = prog };
                var handle = GCHandle.Alloc(ctx);
                try
                {
                    int rc = NativeMethods.bp_dll_archive_extract_all(ref io,
                        ExtractOpenCb, ExtractDoneCb, ExtractProgressCb,
                        GCHandle.ToIntPtr(handle));

                    if (rc != NativeMethods.BP_OK && rc != NativeMethods.BP_ERR_ABORTED)
                        MessageBox.Show(this, "Extraction error (code " + rc + ")",
                            "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
                finally
                {
                    handle.Free();
                    Enabled = true;
                }
            }
        }

        private class ExtractContext
        {
            public string destDir;
            public ProgressForm form;
        }

        /* Extract callbacks -- these are called from native code */

        private static readonly NativeMethods.BpExtractOpenDelegate ExtractOpenCb = ExtractOpen;
        private static readonly NativeMethods.BpExtractDoneDelegate ExtractDoneCb = ExtractDone;
        private static readonly BpProgressDelegate ExtractProgressCb = ExtractProgress;

        private static IntPtr ExtractOpen(ref BpEntry entry, IntPtr ud)
        {
            var ctx = (ExtractContext)GCHandle.FromIntPtr(ud).Target;
            string name = entry.name != IntPtr.Zero
                ? Marshal.PtrToStringAnsi(entry.name) : null;
            if (name == null) return IntPtr.Zero;

            /* Skip resource/meta entries */
            if (entry.entry_type == NativeMethods.BP_TYPE_RSRC ||
                entry.entry_type == NativeMethods.BP_TYPE_META)
                return IntPtr.Zero;

            string fullPath = Path.Combine(ctx.destDir,
                name.Replace('/', Path.DirectorySeparatorChar));

            /* Prevent path traversal */
            string canonical = Path.GetFullPath(fullPath);
            if (!canonical.StartsWith(Path.GetFullPath(ctx.destDir),
                    StringComparison.OrdinalIgnoreCase))
                return IntPtr.Zero;

            if (entry.entry_type == NativeMethods.BP_TYPE_DIR)
            {
                Directory.CreateDirectory(fullPath);
                return IntPtr.Zero;
            }

            string dir = Path.GetDirectoryName(fullPath);
            if (!string.IsNullOrEmpty(dir))
                Directory.CreateDirectory(dir);

            IntPtr fp = NativeMethods.bp_dll_fopen(fullPath, "wb");
            if (fp == IntPtr.Zero) return IntPtr.Zero;

            /* Allocate a bp_io_t on unmanaged heap, fill it, return pointer */
            int size = Marshal.SizeOf(typeof(BpIo));
            IntPtr ioPtr = Marshal.AllocHGlobal(size);
            BpIo fileIo = new BpIo();
            NativeMethods.bp_dll_io_from_file(ref fileIo, fp);
            Marshal.StructureToPtr(fileIo, ioPtr, false);
            return ioPtr;
        }

        private static void ExtractDone(ref BpEntry entry, IntPtr outIo, IntPtr ud)
        {
            if (outIo == IntPtr.Zero) return;
            BpIo io = (BpIo)Marshal.PtrToStructure(outIo, typeof(BpIo));
            if (io.handle != IntPtr.Zero)
                NativeMethods.bp_dll_fclose(io.handle);
            Marshal.FreeHGlobal(outIo);
        }

        private static int ExtractProgress(ref BpProgress progress, IntPtr ud)
        {
            var ctx = (ExtractContext)GCHandle.FromIntPtr(ud).Target;
            string name = progress.file_name != IntPtr.Zero
                ? Marshal.PtrToStringAnsi(progress.file_name) : "";

            double total = progress.archive_orig_total.ToDouble();
            double done = progress.archive_orig_done.ToDouble()
                        + progress.file_bytes_done.ToDouble();
            int pct = total > 0 ? (int)(done / total * 100) : 0;
            if (pct > 100) pct = 100;

            ctx.form.UpdateProgress(name, pct, (int)progress.file_index,
                                     (int)progress.file_count);
            Application.DoEvents();
            return ctx.form.Cancelled ? 1 : 0;
        }

        /* ---------------------------------------------------------------- */
        /*  Validate                                                        */
        /* ---------------------------------------------------------------- */

        private void DoValidate()
        {
            NativeMethods.bp_dll_fseek_start(archiveFp);

            BpIo io = new BpIo();
            NativeMethods.bp_dll_io_from_file(ref io, archiveFp);

            using (var prog = new ProgressForm("Validating archive..."))
            {
                uint fileTotal = archiveInfo.entry_count - archiveInfo.dir_count;
                string initialStatus = string.Format(
                    "{0} files, {1} directories; preparing validation...",
                    fileTotal, archiveInfo.dir_count);

                prog.UpdateProgress("Preparing validation...", 0, initialStatus);
                prog.Show(this);
                Enabled = false;

                var ctx = new ValidateContext
                {
                    form = prog,
                    fileTotal = fileTotal,
                    dirTotal = archiveInfo.dir_count,
                    entryTotal = archiveInfo.entry_count,
                    log = new StringBuilder()
                };
                var handle = GCHandle.Alloc(ctx);

                try
                {
                    int rc = NativeMethods.bp_dll_archive_test_progress(ref io,
                        ValidateCb, GCHandle.ToIntPtr(handle),
                        ValidateProgressCb, GCHandle.ToIntPtr(handle));

                    if (rc == NativeMethods.BP_ERR_ABORTED)
                    {
                        Enabled = true;
                        MessageBox.Show(this, "Validation cancelled.",
                            "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Information);
                        return;
                    }

                    if (rc != NativeMethods.BP_OK && rc != NativeMethods.BP_ERR_CHECKSUM)
                    {
                        Enabled = true;
                        MessageBox.Show(this, "Validation error (code " + rc + ")",
                            "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }

                    string summary;
                    if (ctx.failCount == 0)
                    {
                        summary = string.Format("{0} entr{1} tested; all OK",
                            ctx.okCount, ctx.okCount == 1 ? "y" : "ies");
                    }
                    else
                    {
                        summary = string.Format("{0} OK, {1} FAILED",
                            ctx.okCount, ctx.failCount);
                    }

                    Enabled = true;
                    ShowTextDialog("Validate Archive",
                        summary + "\r\n\r\n" + ctx.log.ToString());
                }
                finally
                {
                    handle.Free();
                    Enabled = true;
                }
            }
        }

        private class ValidateContext
        {
            public ProgressForm form;
            public int okCount;
            public int failCount;
            public uint fileTotal;
            public uint dirTotal;
            public uint entryTotal;
            public StringBuilder log;
        }

        private static readonly NativeMethods.BpTestDelegate ValidateCb = ValidateEntry;
        private static readonly BpProgressDelegate ValidateProgressCb = ValidateProgress;

        private static void ValidateEntry(ref BpEntry entry, int result, IntPtr ud)
        {
            var ctx = (ValidateContext)GCHandle.FromIntPtr(ud).Target;
            string name = entry.name != IntPtr.Zero
                ? Marshal.PtrToStringAnsi(entry.name) : "?";

            if (result == NativeMethods.BP_OK)
            {
                ctx.okCount++;
                ctx.log.Append("  OK      ");
                ctx.log.Append(name);
                ctx.log.Append("\r\n");
            }
            else
            {
                ctx.failCount++;
                ctx.log.Append("  FAILED  ");
                ctx.log.Append(name);
                ctx.log.Append("  (code ");
                ctx.log.Append(result);
                ctx.log.Append(")\r\n");
            }
        }

        private static int ValidateProgress(ref BpProgress progress, IntPtr ud)
        {
            var ctx = (ValidateContext)GCHandle.FromIntPtr(ud).Target;
            string name = progress.file_name != IntPtr.Zero
                ? Marshal.PtrToStringAnsi(progress.file_name) : "";
            double total = progress.archive_orig_total.ToDouble();
            double done = progress.archive_orig_done.ToDouble();
            int pct = total > 0 ? (int)(done / total * 100.0) : 0;
            uint entryTotal = ctx.entryTotal != 0 ? ctx.entryTotal : progress.file_count;
            string status = string.Format(
                "{0} files, {1} directories; validating entry {2} of {3}",
                ctx.fileTotal, ctx.dirTotal, progress.file_index + 1, entryTotal);

            if (pct > 100) pct = 100;
            ctx.form.UpdateProgress(name, pct, status);
            Application.DoEvents();
            return ctx.form.Cancelled ? 1 : 0;
        }

        /* ---------------------------------------------------------------- */
        /*  Create                                                          */
        /* ---------------------------------------------------------------- */

        private void DoCreate(string[] sourcePaths)
        {
            string outPath;
            using (var dlg = new SaveFileDialog())
            {
                dlg.Filter = "BoltPack archive (*.bolt)|*.bolt";
                dlg.DefaultExt = "bolt";
                dlg.FileName = "archive.bolt";
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                outPath = dlg.FileName;
            }

            /* Gather all files recursively */
            var files = new List<string>();
            string baseDir = null;

            foreach (string p in sourcePaths)
            {
                if (Directory.Exists(p))
                {
                    if (baseDir == null)
                        baseDir = Path.GetDirectoryName(p);
                    GatherFiles(p, files);
                }
                else if (File.Exists(p))
                {
                    if (baseDir == null)
                        baseDir = Path.GetDirectoryName(p);
                    files.Add(p);
                }
            }

            if (files.Count == 0)
            {
                MessageBox.Show(this, "No files to archive.",
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            if (baseDir == null) baseDir = "";

            IntPtr outFp = NativeMethods.bp_dll_fopen(outPath, "wb");
            if (outFp == IntPtr.Zero)
            {
                MessageBox.Show(this, "Cannot create output file.",
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            using (var prog = new ProgressForm("Creating archive..."))
            {
                prog.Show(this);
                Enabled = false;

                try
                {
                    BpIo outIo = new BpIo();
                    NativeMethods.bp_dll_io_from_file(ref outIo, outFp);

                    /* Pin the bp_io_t so the native side can use it */
                    GCHandle ioHandle = GCHandle.Alloc(outIo, GCHandleType.Pinned);

                    /* We need a bp_archive_t on unmanaged heap */
                    int arSize = 256; /* generous size for bp_archive_t */
                    IntPtr arPtr = Marshal.AllocHGlobal(arSize);
                    for (int i = 0; i < arSize; i++)
                        Marshal.WriteByte(arPtr, i, 0);

                    int rc = NativeMethods.bp_dll_archive_create(arPtr,
                        ioHandle.AddrOfPinnedObject(), 0, NativeMethods.BP_METHOD_FASTLZ);

                    if (rc != NativeMethods.BP_OK)
                    {
                        MessageBox.Show(this, "Failed to initialize archive.",
                            "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        Marshal.FreeHGlobal(arPtr);
                        ioHandle.Free();
                        return;
                    }

                    for (int i = 0; i < files.Count; i++)
                    {
                        string relPath = files[i];
                        if (baseDir.Length > 0 && relPath.StartsWith(baseDir,
                                StringComparison.OrdinalIgnoreCase))
                            relPath = relPath.Substring(baseDir.Length)
                                             .TrimStart(Path.DirectorySeparatorChar);
                        relPath = relPath.Replace(Path.DirectorySeparatorChar, '/');

                        long fileLen = new FileInfo(files[i]).Length;
                        BpUint64 fsize = BpUint64.FromUlong((ulong)fileLen);
                        BpUint64 mtime = BpUint64.FromUlong(
                            (ulong)new DateTimeOffset(File.GetLastWriteTimeUtc(files[i]))
                                .ToUnixTimeSeconds());

                        IntPtr fileFp = NativeMethods.bp_dll_fopen(files[i], "rb");
                        if (fileFp == IntPtr.Zero) continue;

                        BpIo fileIo = new BpIo();
                        NativeMethods.bp_dll_io_from_file(ref fileIo, fileFp);
                        GCHandle fioHandle = GCHandle.Alloc(fileIo, GCHandleType.Pinned);

                        rc = NativeMethods.bp_dll_archive_add_entry(arPtr, relPath,
                            NativeMethods.BP_TYPE_DATA, NativeMethods.BP_OS_WINDOWS,
                            fioHandle.AddrOfPinnedObject(), fsize, mtime);

                        fioHandle.Free();
                        NativeMethods.bp_dll_fclose(fileFp);

                        if (rc != NativeMethods.BP_OK) break;

                        int pct = (int)((i + 1) * 100L / files.Count);
                        prog.UpdateProgress(relPath, pct, i + 1, files.Count);
                        Application.DoEvents();
                        if (prog.Cancelled) break;
                    }

                    if (!prog.Cancelled)
                        NativeMethods.bp_dll_archive_finalize(arPtr);

                    NativeMethods.bp_dll_archive_close(arPtr);
                    Marshal.FreeHGlobal(arPtr);
                    ioHandle.Free();
                }
                finally
                {
                    NativeMethods.bp_dll_fclose(outFp);
                    Enabled = true;
                }
            }

            /* Open the newly created archive */
            if (File.Exists(outPath))
                DoOpen(outPath);
        }

        private static void GatherFiles(string dir, List<string> files)
        {
            foreach (string f in Directory.GetFiles(dir))
                files.Add(f);
            foreach (string d in Directory.GetDirectories(dir))
                GatherFiles(d, files);
        }

        private void ShowTextDialog(string title, string body)
        {
            using (var dlg = new Form())
            {
                dlg.Text = title;
                dlg.StartPosition = FormStartPosition.CenterParent;
                dlg.Size = new Size(620, 460);
                dlg.MinimumSize = new Size(420, 300);

                var textBox = new TextBox
                {
                    Multiline = true,
                    ReadOnly = true,
                    ScrollBars = ScrollBars.Both,
                    WordWrap = false,
                    Font = new Font("Consolas", 9),
                    Text = body,
                    Location = new Point(12, 12),
                    Size = new Size(580, 370),
                    Anchor = AnchorStyles.Top | AnchorStyles.Bottom |
                             AnchorStyles.Left | AnchorStyles.Right
                };

                var closeButton = new Button
                {
                    Text = "Close",
                    DialogResult = DialogResult.OK,
                    Size = new Size(80, 28),
                    Location = new Point(260, 392),
                    Anchor = AnchorStyles.Bottom
                };

                dlg.Controls.Add(textBox);
                dlg.Controls.Add(closeButton);
                dlg.AcceptButton = closeButton;
                dlg.CancelButton = closeButton;
                dlg.ShowDialog(this);
            }
        }

        /* ================================================================ */
        /*  Helpers                                                         */
        /* ================================================================ */

        private static string FormatSize(double bytes)
        {
            if (bytes < 1024) return ((long)bytes).ToString() + " B";
            if (bytes < 1024 * 1024) return (bytes / 1024).ToString("0.0") + " KB";
            if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).ToString("0.0") + " MB";
            return (bytes / (1024 * 1024 * 1024)).ToString("0.0") + " GB";
        }

        private static string FormatTime(BpUint64 mtime)
        {
            double secs = mtime.ToDouble();
            if (secs <= 0) return "-";
            try
            {
                var dt = DateTimeOffset.FromUnixTimeSeconds((long)secs).LocalDateTime;
                return dt.ToString("yyyy-MM-dd HH:mm");
            }
            catch
            {
                return "-";
            }
        }
    }
}
