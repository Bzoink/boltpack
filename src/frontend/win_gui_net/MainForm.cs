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
        private static readonly UTF8Encoding StrictUtf8 = new UTF8Encoding(false, true);

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
            archiveInfo = NewArchiveInfo();
        }

        private void DoOpen(string path)
        {
            CloseArchive();
            listView.Items.Clear();

            archiveFp = NativeMethods.bp_dll_fopen_w(path, "rb");
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

            archiveInfo = NewArchiveInfo();
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

            var entries = new List<ArchiveEntryView>();
            string listError = null;
            BpListDelegate listCb = delegate (ref BpEntry entry, IntPtr ud)
            {
                if (entry.entry_type == NativeMethods.BP_TYPE_META ||
                    entry.entry_type == NativeMethods.BP_TYPE_RSRC)
                    return 0;
                try
                {
                    entries.Add(new ArchiveEntryView
                    {
                        name = DecodeEntryName(entry),
                        original_size = entry.original_size,
                        compressed_size = entry.compressed_size,
                        method = entry.method,
                        mtime = entry.mtime,
                        flags = entry.flags,
                        entry_type = entry.entry_type
                    });
                }
                catch (Exception ex) { listError = ex.Message; }
                return 0;
            };

            rc = NativeMethods.bp_dll_archive_list(ref io, listCb, IntPtr.Zero);
            GC.KeepAlive(listCb);
            if (rc != NativeMethods.BP_OK || listError != null)
            {
                MessageBox.Show(this, "Error reading archive contents" +
                    (listError == null ? "." : ":\r\n" + listError),
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                CloseArchive();
                return;
            }

            listView.BeginUpdate();
            foreach (var ent in entries)
            {
                string name = ent.name;
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
                item.SubItems.Add(FormatTime(ent.mtime, ent.flags));

                if (ent.entry_type == NativeMethods.BP_TYPE_DIR)
                    item.ForeColor = Color.DarkBlue;

                listView.Items.Add(item);
            }
            listView.EndUpdate();

            welcomeLabel.Visible = false;
            listView.Visible = true;
            Text = "BoltPack - " + Path.GetFileName(path);
            statusLabel.Text = string.Format("{0} entries, {1} total",
                entries.Count, FormatSize(archiveInfo.total_original.ToDouble()));

            /* prevent the callback delegate from being collected */
            GC.KeepAlive(listCb);
        }

        /* ---------------------------------------------------------------- */
        /*  Extract                                                         */
        /* ---------------------------------------------------------------- */

        private void DoExtract(string destDir)
        {
            ExtractContext ctx;
            string preflightError;
            if (!TryPrepareExtraction(destDir, out ctx, out preflightError))
            {
                MessageBox.Show(this, "Cannot safely extract this archive:\r\n" +
                    preflightError, "BoltPack", MessageBoxButtons.OK,
                    MessageBoxIcon.Error);
                return;
            }

            NativeMethods.bp_dll_fseek_start(archiveFp);

            BpIo io = new BpIo();
            NativeMethods.bp_dll_io_from_file(ref io, archiveFp);

            using (var prog = new ProgressForm("Extracting..."))
            {
                prog.Show(this);
                Enabled = false;

                ctx.form = prog;
                var handle = GCHandle.Alloc(ctx);
                try
                {
                    int rc = NativeMethods.bp_dll_archive_extract_all_ex(ref io,
                        ExtractOpenCb, ExtractDoneStatusCb, ExtractProgressCb,
                        GCHandle.ToIntPtr(handle));

                    if (rc != NativeMethods.BP_OK && rc != NativeMethods.BP_ERR_PARTIAL)
                        CleanupUnverifiedCreatedDirectories(ctx);
                    FinalizeDirectories(ctx);
                    if (rc != NativeMethods.BP_OK && rc != NativeMethods.BP_ERR_PARTIAL &&
                        rc != NativeMethods.BP_ERR_ABORTED)
                        MessageBox.Show(this, "Extraction error (code " + rc + ")",
                            "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    else if (rc == NativeMethods.BP_ERR_ABORTED)
                        MessageBox.Show(this, "Extraction was cancelled. Verified files " +
                            "already published remain in the destination.", "BoltPack",
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    else if (rc == NativeMethods.BP_ERR_PARTIAL || ctx.partial)
                        MessageBox.Show(this, "Extraction completed with files or metadata " +
                            "that Windows could not preserve.", "BoltPack",
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
                finally
                {
                    handle.Free();
                    Enabled = true;
                }
            }
        }

        private sealed class ArchiveEntryView
        {
            public string name;
            public BpUint64 original_size;
            public BpUint64 compressed_size;
            public byte method;
            public BpUint64 mtime;
            public ushort flags;
            public byte entry_type;
        }

        private sealed class PendingFat
        {
            public byte valid;
            public byte value;
        }

        private class ExtractContext
        {
            public string root;
            public string rootPrefix;
            public ProgressForm form;
            public bool partial;
            public string preflightError;
            public readonly Dictionary<string, byte> paths =
                new Dictionary<string, byte>(StringComparer.OrdinalIgnoreCase);
            public readonly Dictionary<string, byte> subjectTypes =
                new Dictionary<string, byte>(StringComparer.OrdinalIgnoreCase);
            public readonly HashSet<string> existingFiles =
                new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            public readonly HashSet<string> directories =
                new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            public readonly HashSet<string> createdDirectories =
                new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            public readonly Dictionary<string, ulong> directoryTimes =
                new Dictionary<string, ulong>(StringComparer.OrdinalIgnoreCase);
            public readonly Dictionary<string, PendingFat> directoryFat =
                new Dictionary<string, PendingFat>(StringComparer.OrdinalIgnoreCase);
        }

        private sealed class ExtractOutput
        {
            public ExtractContext context;
            public string name;
            public string target;
            public string temporary;
            public byte entryType;
            public bool mtimeValid;
            public ulong mtime;
            public bool metadata;
            public bool directory;
            public bool replaceExisting;
            public FileStream file;
            public MemoryStream memory;
            public GCHandle selfHandle;
            public BpWriteDelegate writeDelegate;
            public IntPtr ioPointer;
            public bool published;
        }

        /* Extract callbacks -- these are called from native code */

        private static readonly NativeMethods.BpExtractOpenDelegate ExtractOpenCb = ExtractOpen;
        private static readonly NativeMethods.BpExtractDoneStatusDelegate ExtractDoneStatusCb = ExtractDoneStatus;
        private static readonly BpProgressDelegate ExtractProgressCb = ExtractProgress;

        private bool TryPrepareExtraction(string destination, out ExtractContext ctx,
                                          out string error)
        {
            ctx = new ExtractContext();
            error = null;
            try
            {
                ctx.root = Path.GetFullPath(destination);
                if (!Directory.Exists(ctx.root))
                    throw new IOException("The destination directory does not exist.");
                VerifyDirectoryChain(ctx.root);
                ctx.rootPrefix = ctx.root.EndsWith(Path.DirectorySeparatorChar.ToString(),
                    StringComparison.Ordinal) ? ctx.root :
                    ctx.root + Path.DirectorySeparatorChar;

                NativeMethods.bp_dll_fseek_start(archiveFp);
                BpIo io = new BpIo();
                NativeMethods.bp_dll_io_from_file(ref io, archiveFp);
                BpListDelegate preflight = delegate (ref BpEntry entry, IntPtr userData)
                {
                    try
                    {
                        if (entry.entry_type == NativeMethods.BP_TYPE_DATA ||
                            entry.entry_type == NativeMethods.BP_TYPE_DIR ||
                            entry.entry_type == NativeMethods.BP_TYPE_SYMLINK)
                        {
                            string name = DecodeEntryName(entry);
                            string relative = ValidateWindowsArchiveName(name,
                                entry.entry_type == NativeMethods.BP_TYPE_DIR);
                            string key = relative.Normalize(NormalizationForm.FormC)
                                .Replace('/', '\\');
                            foreach (var prior in ctx.paths)
                            {
                                if (string.Equals(prior.Key, key,
                                        StringComparison.OrdinalIgnoreCase))
                                    throw new IOException("Two archive entries map to the same Windows path: " + name);
                                if (prior.Value != NativeMethods.BP_TYPE_DIR &&
                                    key.StartsWith(prior.Key + "\\",
                                             StringComparison.OrdinalIgnoreCase))
                                    throw new IOException("A file conflicts with a child path: " + name);
                                if (entry.entry_type != NativeMethods.BP_TYPE_DIR &&
                                    prior.Key.StartsWith(key + "\\",
                                        StringComparison.OrdinalIgnoreCase))
                                    throw new IOException("A file conflicts with an existing child path: " + name);
                            }
                            ctx.paths.Add(key, entry.entry_type);
                            ctx.subjectTypes.Add(key, entry.entry_type);

                            string full = GetOutputPath(ctx, relative);
                            if (full.Length >= 260)
                                throw new IOException("A path exceeds the Windows MAX_PATH limit: " + name);
                            if (entry.entry_type == NativeMethods.BP_TYPE_DATA &&
                                full.Length + 46 >= 260)
                                throw new IOException("A path leaves no room for the required sibling staging file: " + name);
                            VerifyExistingOutputPath(ctx, relative,
                                entry.entry_type == NativeMethods.BP_TYPE_DIR);
                            if (entry.entry_type == NativeMethods.BP_TYPE_DATA)
                            {
                                FileAttributes existing;
                                if (TryGetAttributes(full, out existing))
                                    ctx.existingFiles.Add(key);
                            }
                            if (entry.entry_type == NativeMethods.BP_TYPE_SYMLINK ||
                                (entry.flags & NativeMethods.BP_ENTRY_FLAG_MODE_VALID) != 0)
                                ctx.partial = true;
                        }
                        else if (entry.entry_type == NativeMethods.BP_TYPE_RSRC ||
                                 (entry.entry_type == NativeMethods.BP_TYPE_META &&
                                  entry.platform != NativeMethods.BP_META_PORTABLE))
                            ctx.partial = true;
                    }
                    catch (Exception ex)
                    {
                        if (ctx.preflightError == null) ctx.preflightError = ex.Message;
                    }
                    return 0;
                };

                int rc = NativeMethods.bp_dll_archive_list(ref io, preflight, IntPtr.Zero);
                GC.KeepAlive(preflight);
                if (rc != NativeMethods.BP_OK)
                    throw new IOException("Archive listing failed (code " + rc + ").");
                if (ctx.preflightError != null)
                    throw new IOException(ctx.preflightError);
                return true;
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
        }

        private static IntPtr ExtractOpen(ref BpEntry entry, IntPtr ud)
        {
            var ctx = (ExtractContext)GCHandle.FromIntPtr(ud).Target;
            ExtractOutput output = null;
            try
            {
                string name = DecodeEntryName(entry);
                if (entry.entry_type == NativeMethods.BP_TYPE_RSRC ||
                    entry.entry_type == NativeMethods.BP_TYPE_SYMLINK)
                {
                    ctx.partial = true;
                    return IntPtr.Zero;
                }
                if (entry.entry_type == NativeMethods.BP_TYPE_META)
                {
                    if (entry.platform != NativeMethods.BP_META_PORTABLE)
                    {
                        ctx.partial = true;
                        return IntPtr.Zero;
                    }
                    output = new ExtractOutput
                    {
                        context = ctx,
                        name = name,
                        metadata = true,
                        memory = new MemoryStream()
                    };
                    return MakeManagedOutput(output);
                }
                if (entry.entry_type == NativeMethods.BP_TYPE_DIR)
                {
                    string relative = ValidateWindowsArchiveName(name, true);
                    string full = GetOutputPath(ctx, relative);
                    EnsureDirectoriesNoReparse(ctx, relative, true);
                    output = new ExtractOutput
                    {
                        context = ctx,
                        name = name,
                        target = full,
                        entryType = entry.entry_type,
                        directory = true,
                        mtimeValid = (entry.flags & NativeMethods.BP_ENTRY_FLAG_MTIME_VALID) != 0,
                        mtime = entry.mtime.ToUlong()
                    };
                    if ((entry.flags & NativeMethods.BP_ENTRY_FLAG_MODE_VALID) != 0)
                        ctx.partial = true;
                    return MakeManagedOutput(output);
                }
                if (entry.entry_type != NativeMethods.BP_TYPE_DATA)
                {
                    ctx.partial = true;
                    return IntPtr.Zero;
                }

                string fileRelative = ValidateWindowsArchiveName(name, false);
                EnsureDirectoriesNoReparse(ctx, fileRelative, false);
                string target = GetOutputPath(ctx, fileRelative);
                VerifyExistingLeaf(target);
                string temporary = null;
                FileStream file = null;
                for (int attempt = 0; attempt < 8; attempt++)
                {
                    temporary = target + ".boltpack-" + Guid.NewGuid().ToString("N") + ".tmp";
                    try
                    {
                        file = new FileStream(temporary, FileMode.CreateNew,
                            FileAccess.Write, FileShare.None, 65536,
                            FileOptions.SequentialScan);
                        break;
                    }
                    catch (IOException)
                    {
                        file = null;
                    }
                }
                if (file == null) throw new IOException("Cannot create a unique sibling temporary file.");
                output = new ExtractOutput
                {
                    context = ctx,
                    name = name,
                    target = target,
                    temporary = temporary,
                    entryType = entry.entry_type,
                    file = file,
                    mtimeValid = (entry.flags & NativeMethods.BP_ENTRY_FLAG_MTIME_VALID) != 0,
                    mtime = entry.mtime.ToUlong(),
                    replaceExisting = ctx.existingFiles.Contains(
                        fileRelative.Normalize(NormalizationForm.FormC).Replace('/', '\\'))
                };
                if ((entry.flags & NativeMethods.BP_ENTRY_FLAG_MODE_VALID) != 0)
                    ctx.partial = true;
                return MakeManagedOutput(output);
            }
            catch
            {
                ctx.partial = true;
                if (output != null) ReleaseManagedOutput(output, true);
                return IntPtr.Zero;
            }
        }

        private static IntPtr MakeManagedOutput(ExtractOutput output)
        {
            output.selfHandle = GCHandle.Alloc(output);
            output.writeDelegate = ManagedWrite;
            BpIo io = new BpIo();
            io.write = Marshal.GetFunctionPointerForDelegate(output.writeDelegate);
            io.handle = GCHandle.ToIntPtr(output.selfHandle);
            output.ioPointer = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(BpIo)));
            Marshal.StructureToPtr(io, output.ioPointer, false);
            return output.ioPointer;
        }

        private static int ManagedWrite(IntPtr handle, IntPtr buffer, uint count)
        {
            if (count > int.MaxValue) return -1;
            try
            {
                var output = (ExtractOutput)GCHandle.FromIntPtr(handle).Target;
                byte[] bytes = new byte[(int)count];
                if (count != 0) Marshal.Copy(buffer, bytes, 0, (int)count);
                if (output.file != null) output.file.Write(bytes, 0, bytes.Length);
                else if (output.memory != null) output.memory.Write(bytes, 0, bytes.Length);
                else return -1;
                return (int)count;
            }
            catch
            {
                return -1;
            }
        }

        private static int ExtractDoneStatus(ref BpEntry entry, IntPtr outIo,
                                             int status, IntPtr ud)
        {
            if (outIo == IntPtr.Zero) return NativeMethods.BP_OK;
            BpIo io = (BpIo)Marshal.PtrToStructure(outIo, typeof(BpIo));
            var output = (ExtractOutput)GCHandle.FromIntPtr(io.handle).Target;
            int result = NativeMethods.BP_OK;
            try
            {
                if (status != NativeMethods.BP_OK) return NativeMethods.BP_OK;
                if (output.metadata)
                    return ApplyPortableMetadata(output, output.memory.ToArray());
                if (output.directory)
                {
                    output.context.directories.Add(output.target);
                    if (output.mtimeValid)
                        output.context.directoryTimes[output.target] = output.mtime;
                    return NativeMethods.BP_OK;
                }

                output.file.Flush(true);
                output.file.Dispose();
                output.file = null;
                string relative = ValidateWindowsArchiveName(output.name, false);
                VerifyExistingOutputPath(output.context, relative, false);
                VerifyExistingLeaf(output.target);
                FileAttributes currentTarget;
                bool targetExists = TryGetAttributes(output.target, out currentTarget);
                if (targetExists && !output.replaceExisting)
                    return NativeMethods.BP_ERR_IO;
                if (!NativeMethods.MoveFileEx(output.temporary, output.target,
                        (output.replaceExisting ? NativeMethods.MOVEFILE_REPLACE_EXISTING : 0) |
                        NativeMethods.MOVEFILE_WRITE_THROUGH))
                    return NativeMethods.BP_ERR_IO;
                output.published = true;
                if (output.mtimeValid && !ApplyFileTime(output.target, output.mtime))
                    output.context.partial = true;
            }
            catch
            {
                if (output.published)
                {
                    output.context.partial = true;
                    result = NativeMethods.BP_ERR_PARTIAL;
                }
                else result = NativeMethods.BP_ERR_IO;
            }
            finally
            {
                ReleaseManagedOutput(output, !output.published);
            }
            return result;
        }

        private static int ExtractProgress(ref BpProgress progress, IntPtr ud)
        {
            var ctx = (ExtractContext)GCHandle.FromIntPtr(ud).Target;
            string name = DecodeUtf8Z(progress.file_name);

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

        private static string DecodeEntryName(BpEntry entry)
        {
            if (entry.name == IntPtr.Zero || entry.name_len == 0)
                throw new InvalidDataException("The archive contains an empty entry name.");
            byte[] bytes = new byte[entry.name_len];
            Marshal.Copy(entry.name, bytes, 0, bytes.Length);
            return StrictUtf8.GetString(bytes);
        }

        private static string DecodeUtf8Z(IntPtr pointer)
        {
            if (pointer == IntPtr.Zero) return "";
            try
            {
                var bytes = new List<byte>();
                for (int i = 0; i < 65536; i++)
                {
                    byte value = Marshal.ReadByte(pointer, i);
                    if (value == 0) return StrictUtf8.GetString(bytes.ToArray());
                    bytes.Add(value);
                }
            }
            catch { }
            return "(invalid UTF-8 name)";
        }

        private static string ValidateWindowsArchiveName(string name, bool directory)
        {
            if (string.IsNullOrEmpty(name) || name.IndexOf('\\') >= 0 ||
                name[0] == '/' || name.IndexOf(':') >= 0)
                throw new IOException("The archive contains an unsafe Windows path.");
            if (directory)
            {
                if (!name.EndsWith("/", StringComparison.Ordinal))
                    throw new IOException("A directory entry lacks its trailing slash.");
                name = name.Substring(0, name.Length - 1);
            }
            else if (name.EndsWith("/", StringComparison.Ordinal))
                throw new IOException("A file entry ends with a slash.");

            string[] components = name.Split('/');
            foreach (string component in components)
            {
                if (component.Length == 0 || component == "." || component == ".." ||
                    component.EndsWith(".", StringComparison.Ordinal) ||
                    component.EndsWith(" ", StringComparison.Ordinal))
                    throw new IOException("The archive contains an unsafe Windows path component.");
                foreach (char ch in component)
                {
                    if (ch < 32 || "<>:\"|?*".IndexOf(ch) >= 0 ||
                        Array.IndexOf(Path.GetInvalidFileNameChars(), ch) >= 0)
                        throw new IOException("A name cannot be represented by Windows: " + component);
                }

                string deviceBase = component.Split('.')[0].TrimEnd(' ', '.');
                if (string.Equals(deviceBase, "CON", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(deviceBase, "PRN", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(deviceBase, "AUX", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(deviceBase, "NUL", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(deviceBase, "CONIN$", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(deviceBase, "CONOUT$", StringComparison.OrdinalIgnoreCase) ||
                    IsNumberedDevice(deviceBase, "COM") || IsNumberedDevice(deviceBase, "LPT"))
                    throw new IOException("A reserved Windows device name cannot be extracted: " + component);
            }
            return name;
        }

        private static bool IsNumberedDevice(string name, string prefix)
        {
            return name.Length == 4 &&
                name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
                name[3] >= '1' && name[3] <= '9';
        }

        private static string GetOutputPath(ExtractContext ctx, string relative)
        {
            string full = Path.GetFullPath(Path.Combine(ctx.root,
                relative.Replace('/', Path.DirectorySeparatorChar)));
            if (!full.StartsWith(ctx.rootPrefix, StringComparison.OrdinalIgnoreCase))
                throw new IOException("An archive path escapes the selected destination.");
            return full;
        }

        private static void VerifyDirectoryChain(string path)
        {
            string full = Path.GetFullPath(path);
            string current = Path.GetPathRoot(full);
            string remainder = full.Substring(current.Length);
            foreach (string component in remainder.Split(
                         new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                         StringSplitOptions.RemoveEmptyEntries))
            {
                current = Path.Combine(current, component);
                FileAttributes attributes = File.GetAttributes(current);
                if ((attributes & FileAttributes.ReparsePoint) != 0 ||
                    (attributes & FileAttributes.Directory) == 0)
                    throw new IOException("A destination directory component is a reparse point or is not a directory: " + current);
            }
        }

        private static bool TryGetAttributes(string path, out FileAttributes attributes)
        {
            try
            {
                attributes = File.GetAttributes(path);
                return true;
            }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
            attributes = 0;
            return false;
        }

        private static void VerifyExistingOutputPath(ExtractContext ctx,
                                                     string relative, bool directory)
        {
            string current = ctx.root;
            string[] components = relative.Split('/');
            for (int i = 0; i < components.Length; i++)
            {
                current = Path.Combine(current, components[i]);
                FileAttributes attributes;
                if (!TryGetAttributes(current, out attributes)) return;
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("An archive output path crosses an existing reparse point: " + current);
                bool expectDirectory = i < components.Length - 1 || directory;
                if (expectDirectory != ((attributes & FileAttributes.Directory) != 0))
                    throw new IOException("An existing destination object conflicts with an archive path: " + current);
            }
        }

        private static void EnsureDirectoriesNoReparse(ExtractContext ctx,
                                                       string relative, bool includeLeaf)
        {
            string[] components = relative.Split('/');
            int count = includeLeaf ? components.Length : components.Length - 1;
            string current = ctx.root;
            for (int i = 0; i < count; i++)
            {
                current = Path.Combine(current, components[i]);
                FileAttributes attributes;
                if (!TryGetAttributes(current, out attributes))
                {
                    Directory.CreateDirectory(current);
                    attributes = File.GetAttributes(current);
                    ctx.createdDirectories.Add(current);
                }
                if ((attributes & FileAttributes.ReparsePoint) != 0 ||
                    (attributes & FileAttributes.Directory) == 0)
                    throw new IOException("An output directory is a reparse point or is not a directory: " + current);
            }
        }

        private static void VerifyExistingLeaf(string path)
        {
            FileAttributes attributes;
            if (!TryGetAttributes(path, out attributes)) return;
            if ((attributes & FileAttributes.ReparsePoint) != 0 ||
                (attributes & FileAttributes.Directory) != 0)
                throw new IOException("The output file conflicts with an existing directory or reparse point: " + path);
        }

        private static bool ApplyFileTime(string path, ulong seconds)
        {
            try
            {
                if (seconds > (ulong)long.MaxValue) return false;
                DateTime value = DateTimeOffset.FromUnixTimeSeconds((long)seconds).UtcDateTime;
                File.SetLastWriteTimeUtc(path, value);
                return true;
            }
            catch { return false; }
        }

        private static int ApplyPortableMetadata(ExtractOutput output, byte[] data)
        {
            ExtractContext ctx = output.context;
            if (data.Length < 8 || data[0] != 'B' || data[1] != 'P' ||
                data[2] != 'M' || data[3] != 'T')
                return NativeMethods.BP_ERR_CORRUPT;
            if (data[4] != 1)
            {
                ctx.partial = true;
                return NativeMethods.BP_ERR_PARTIAL;
            }

            string name = output.name;
            bool isDirectory = data[5] == NativeMethods.BP_TYPE_DIR;
            if (!isDirectory && data[5] != NativeMethods.BP_TYPE_DATA)
                return NativeMethods.BP_ERR_CORRUPT;
            string relative = ValidateWindowsArchiveName(name, isDirectory);
            string key = relative.Normalize(NormalizationForm.FormC).Replace('/', '\\');
            byte expectedType;
            if (!ctx.subjectTypes.TryGetValue(key, out expectedType) || expectedType != data[5])
                return NativeMethods.BP_ERR_CORRUPT;

            int count = data[6] | (data[7] << 8);
            int position = 8;
            bool hasFat = false;
            bool unknown = false;
            byte valid = 0, value = 0;
            for (int i = 0; i < count; i++)
            {
                if (data.Length - position < 4) return NativeMethods.BP_ERR_CORRUPT;
                int kind = data[position] | (data[position + 1] << 8);
                int length = data[position + 2] | (data[position + 3] << 8);
                position += 4;
                if (length > data.Length - position) return NativeMethods.BP_ERR_CORRUPT;
                if (kind == 1)
                {
                    if (hasFat || length != 2) return NativeMethods.BP_ERR_CORRUPT;
                    valid = data[position];
                    value = data[position + 1];
                    if ((valid & ~0x27) != 0 || (value & ~valid) != 0)
                        return NativeMethods.BP_ERR_CORRUPT;
                    hasFat = true;
                }
                else unknown = true;
                position += length;
            }
            if (position != data.Length) return NativeMethods.BP_ERR_CORRUPT;
            if (unknown) ctx.partial = true;
            if (hasFat)
            {
                string target = GetOutputPath(ctx, relative);
                if (isDirectory)
                {
                    ctx.directoryFat[target] = new PendingFat { valid = valid, value = value };
                    ctx.directories.Add(target);
                }
                else if (!ApplyFatAttributes(target, valid, value))
                    ctx.partial = true;
            }
            return unknown ? NativeMethods.BP_ERR_PARTIAL : NativeMethods.BP_OK;
        }

        private static bool ApplyFatAttributes(string path, byte valid, byte value)
        {
            try
            {
                FileAttributes attributes = File.GetAttributes(path);
                int current = (int)attributes;
                File.SetAttributes(path, (FileAttributes)((current & ~valid) | value));
                return true;
            }
            catch { return false; }
        }

        private static void FinalizeDirectories(ExtractContext ctx)
        {
            var paths = new List<string>(ctx.directories);
            paths.Sort(delegate (string left, string right)
            {
                return right.Length.CompareTo(left.Length);
            });
            foreach (string path in paths)
            {
                try
                {
                    FileAttributes attributes = File.GetAttributes(path);
                    if ((attributes & FileAttributes.Directory) == 0 ||
                        (attributes & FileAttributes.ReparsePoint) != 0)
                    {
                        ctx.partial = true;
                        continue;
                    }
                    ulong seconds;
                    if (ctx.directoryTimes.TryGetValue(path, out seconds) &&
                        !ApplyFileTime(path, seconds)) ctx.partial = true;
                    PendingFat fat;
                    if (ctx.directoryFat.TryGetValue(path, out fat) &&
                        !ApplyFatAttributes(path, fat.valid, fat.value)) ctx.partial = true;
                }
                catch { ctx.partial = true; }
            }
        }

        private static void CleanupUnverifiedCreatedDirectories(ExtractContext ctx)
        {
            var paths = new List<string>(ctx.createdDirectories);
            paths.Sort(delegate (string left, string right)
            {
                return right.Length.CompareTo(left.Length);
            });
            foreach (string path in paths)
            {
                if (ctx.directories.Contains(path)) continue;
                try
                {
                    FileAttributes attributes = File.GetAttributes(path);
                    if ((attributes & FileAttributes.Directory) != 0 &&
                        (attributes & FileAttributes.ReparsePoint) == 0)
                        Directory.Delete(path, false);
                }
                catch { }
            }
        }

        private static void ReleaseManagedOutput(ExtractOutput output, bool deleteTemporary)
        {
            if (output.file != null)
            {
                try { output.file.Dispose(); } catch { }
                output.file = null;
            }
            if (output.memory != null)
            {
                try { output.memory.Dispose(); } catch { }
                output.memory = null;
            }
            if (deleteTemporary && !string.IsNullOrEmpty(output.temporary))
            {
                try { File.Delete(output.temporary); } catch { }
            }
            if (output.ioPointer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(output.ioPointer);
                output.ioPointer = IntPtr.Zero;
            }
            if (output.selfHandle.IsAllocated) output.selfHandle.Free();
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

                    if (rc != NativeMethods.BP_OK && rc != NativeMethods.BP_ERR_CHECKSUM &&
                        rc != NativeMethods.BP_ERR_PARTIAL)
                    {
                        Enabled = true;
                        MessageBox.Show(this, "Validation error (code " + rc + ")",
                            "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }

                    string summary;
                    if (ctx.failCount == 0 && rc == NativeMethods.BP_OK)
                    {
                        summary = string.Format("{0} entr{1} tested; all OK",
                            ctx.okCount, ctx.okCount == 1 ? "y" : "ies");
                    }
                    else if (ctx.failCount == 0)
                    {
                        summary = string.Format("{0} entr{1} tested; one or more metadata items were not understood",
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
            string name;
            try { name = DecodeEntryName(entry); }
            catch { name = "(invalid UTF-8 name)"; }

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
            string name = DecodeUtf8Z(progress.file_name);
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

            var items = new List<CreateItem>();
            var seenSources = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string baseDir = null;
            try
            {
                foreach (string p in sourcePaths)
                {
                    if (Directory.Exists(p))
                    {
                        if (baseDir == null)
                            baseDir = ParentDirectory(Path.GetFullPath(p));
                        GatherEntries(Path.GetFullPath(p), items, seenSources);
                    }
                    else if (File.Exists(p))
                    {
                        if (baseDir == null)
                            baseDir = Path.GetDirectoryName(Path.GetFullPath(p));
                        string full = Path.GetFullPath(p);
                        if (!seenSources.Add(full)) continue;
                        if ((File.GetAttributes(full) & FileAttributes.ReparsePoint) != 0)
                            throw new IOException("Reparse-point source files are not followed by the .NET frontend: " + full);
                        items.Add(new CreateItem { path = full, directory = false });
                    }
                    else
                        throw new IOException("A selected source is missing or cannot be read: " + p);
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Cannot read selected sources:\r\n" + ex.Message,
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (items.Count == 0)
            {
                MessageBox.Show(this, "No files or directories to archive.",
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            if (baseDir == null) baseDir = "";

            string fullOutput = Path.GetFullPath(outPath);
            string outputDirectory = Path.GetDirectoryName(fullOutput);
            string temporary = null;
            IntPtr outFp = IntPtr.Zero;
            bool outputExisted = false;
            try
            {
                VerifyDirectoryChain(outputDirectory);
                FileAttributes existing;
                outputExisted = TryGetAttributes(fullOutput, out existing);
                if (outputExisted && ((existing & FileAttributes.Directory) != 0 ||
                                (existing & FileAttributes.ReparsePoint) != 0))
                    throw new IOException("The archive target is a directory or reparse point.");
                PreflightCreateItems(items, baseDir);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Cannot create this archive safely:\r\n" + ex.Message,
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            for (int attempt = 0; attempt < 8; attempt++)
            {
                string candidate = fullOutput + ".boltpack-" + Guid.NewGuid().ToString("N") + ".tmp";
                if (candidate.Length >= 260)
                {
                    break;
                }
                outFp = NativeMethods.bp_dll_fopen_w_exclusive(candidate);
                if (outFp != IntPtr.Zero) temporary = candidate;
                if (outFp != IntPtr.Zero) break;
            }
            if (outFp == IntPtr.Zero || temporary == null)
            {
                MessageBox.Show(this, "Cannot create a unique sibling archive file.",
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            bool succeeded = false;
            bool cancelled = false;
            bool partial = false;
            bool outputClosed = false;
            int rc = NativeMethods.BP_OK;
            string createError = null;
            try
            {
                using (var prog = new ProgressForm("Creating archive..."))
                {
                    prog.Show(this);
                    Enabled = false;

                    IntPtr arPtr = IntPtr.Zero;
                    GCHandle ioHandle = default(GCHandle);
                    try
                    {
                        BpIo outIo = new BpIo();
                        NativeMethods.bp_dll_io_from_file(ref outIo, outFp);
                        ioHandle = GCHandle.Alloc(outIo, GCHandleType.Pinned);
                        arPtr = NativeMethods.bp_dll_archive_alloc();
                        if (arPtr == IntPtr.Zero) rc = NativeMethods.BP_ERR_NOMEM;
                        if (rc == NativeMethods.BP_OK)
                            rc = NativeMethods.bp_dll_archive_create(arPtr,
                                ioHandle.AddrOfPinnedObject(), 0, NativeMethods.BP_METHOD_BOLTLZ);

                        for (int i = 0; rc == NativeMethods.BP_OK && i < items.Count; i++)
                        {
                            CreateItem item = items[i];
                            string relPath = MakeArchiveName(item.path, baseDir,
                                item.directory);
                            BpUint64 mtime;
                            ushort entryFlags;
                            if (!GetSourceMtime(item.path, out mtime, out entryFlags))
                                partial = true;
                            BpUint64 fileSize = BpUint64.FromUlong(0);
                            IntPtr fileFp = IntPtr.Zero;
                            GCHandle fileIoHandle = default(GCHandle);

                            if (item.directory) relPath += "/";
                            byte[] nameUtf8 = EncodeUtf8Z(relPath);

                            try
                            {
                                if (item.directory)
                                    rc = NativeMethods.bp_dll_archive_add_entry_ex(arPtr,
                                        nameUtf8, NativeMethods.BP_TYPE_DIR,
                                        NativeMethods.BP_OS_WINDOWS, IntPtr.Zero,
                                        fileSize, mtime, entryFlags);
                                else
                                {
                                    FileInfo info = new FileInfo(item.path);
                                    fileSize = BpUint64.FromUlong((ulong)info.Length);
                                    fileFp = NativeMethods.bp_dll_fopen_w(item.path, "rb");
                                    if (fileFp == IntPtr.Zero) rc = NativeMethods.BP_ERR_IO;
                                    else
                                    {
                                        BpIo fileIo = new BpIo();
                                        NativeMethods.bp_dll_io_from_file(ref fileIo, fileFp);
                                        fileIoHandle = GCHandle.Alloc(fileIo, GCHandleType.Pinned);
                                        rc = NativeMethods.bp_dll_archive_add_entry_ex(arPtr,
                                            nameUtf8, NativeMethods.BP_TYPE_DATA,
                                            NativeMethods.BP_OS_WINDOWS,
                                            fileIoHandle.AddrOfPinnedObject(), fileSize,
                                            mtime, entryFlags);
                                    }
                                }

                                if (fileFp != IntPtr.Zero)
                                {
                                    NativeMethods.bp_dll_fclose(fileFp);
                                    fileFp = IntPtr.Zero;
                                }

                                if (rc == NativeMethods.BP_OK)
                                {
                                    FileAttributes attributes = File.GetAttributes(item.path);
                                    rc = NativeMethods.bp_dll_archive_add_fat_metadata(arPtr,
                                        nameUtf8, item.directory
                                            ? NativeMethods.BP_TYPE_DIR : NativeMethods.BP_TYPE_DATA,
                                        0x27, (byte)((int)attributes & 0x27));
                                }
                            }
                            finally
                            {
                                if (fileIoHandle.IsAllocated) fileIoHandle.Free();
                                if (fileFp != IntPtr.Zero) NativeMethods.bp_dll_fclose(fileFp);
                            }
                            if (rc != NativeMethods.BP_OK) break;

                            int pct = (int)((i + 1) * 100L / items.Count);
                            prog.UpdateProgress(relPath, pct, i + 1, items.Count);
                            Application.DoEvents();
                            if (prog.Cancelled)
                            {
                                cancelled = true;
                                break;
                            }
                        }

                        if (rc == NativeMethods.BP_OK && !cancelled)
                            rc = NativeMethods.bp_dll_archive_finalize(arPtr);
                    }
                    finally
                    {
                        if (arPtr != IntPtr.Zero)
                            NativeMethods.bp_dll_archive_free(arPtr);
                        if (ioHandle.IsAllocated) ioHandle.Free();
                        Enabled = true;
                    }
                }
            }
            catch (Exception ex)
            {
                rc = NativeMethods.BP_ERR_IO;
                createError = ex.Message;
            }

            if (outFp != IntPtr.Zero)
            {
                int closeResult = NativeMethods.bp_dll_fclose_checked(outFp);
                outputClosed = true;
                outFp = IntPtr.Zero;
                if (rc == NativeMethods.BP_OK && closeResult != 0)
                    rc = NativeMethods.BP_ERR_IO;
            }

            if (rc == NativeMethods.BP_OK && !cancelled && outputClosed)
            {
                try
                {
                    FileAttributes existing;
                    bool targetExists = TryGetAttributes(fullOutput, out existing);
                    if (targetExists && (!outputExisted ||
                        (existing & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0))
                        throw new IOException("The archive target changed while the new archive was being built.");
                    uint moveFlags = NativeMethods.MOVEFILE_WRITE_THROUGH |
                        (outputExisted ? NativeMethods.MOVEFILE_REPLACE_EXISTING : 0);
                    succeeded = NativeMethods.MoveFileEx(temporary, fullOutput, moveFlags);
                }
                catch (Exception ex) { createError = ex.Message; }
            }
            if (!succeeded)
            {
                try { File.Delete(temporary); } catch { }
                string message = cancelled ? "Archive creation was cancelled." :
                    (rc == NativeMethods.BP_OK ? "Cannot publish the completed archive." :
                     "Archive creation failed (code " + rc + ")." +
                     (createError == null ? "" : "\r\n" + createError));
                MessageBox.Show(this, message, "BoltPack", MessageBoxButtons.OK,
                    cancelled ? MessageBoxIcon.Information : MessageBoxIcon.Error);
                return;
            }

            if (partial)
                MessageBox.Show(this, "The archive was created, but one or more source timestamps were outside the format's supported range and were stored as unknown.",
                    "BoltPack", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            if (File.Exists(fullOutput))
                DoOpen(fullOutput);
        }

        private sealed class CreateItem
        {
            public string path;
            public bool directory;
        }

        private static string ParentDirectory(string path)
        {
            string trimmed = path.TrimEnd(Path.DirectorySeparatorChar,
                                          Path.AltDirectorySeparatorChar);
            return Path.GetDirectoryName(trimmed) ?? Path.GetPathRoot(path);
        }

        private static void GatherEntries(string dir, List<CreateItem> items,
                                          HashSet<string> seen)
        {
            string full = Path.GetFullPath(dir);
            if (!seen.Add(full)) return;
            FileAttributes attributes = File.GetAttributes(full);
            if ((attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Reparse-point directories are not followed by the .NET frontend: " + full);
            items.Add(new CreateItem { path = full, directory = true });
            foreach (string child in Directory.GetDirectories(full))
                GatherEntries(child, items, seen);
            foreach (string file in Directory.GetFiles(full))
            {
                string fileFull = Path.GetFullPath(file);
                if (!seen.Add(fileFull)) continue;
                if ((File.GetAttributes(fileFull) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Reparse-point source files are not followed by the .NET frontend: " + fileFull);
                items.Add(new CreateItem { path = fileFull, directory = false });
            }
        }

        private static string MakeArchiveName(string path, string baseDir,
                                              bool directory)
        {
            string full = Path.GetFullPath(path);
            string relative;
            if (!string.IsNullOrEmpty(baseDir))
            {
                string root = Path.GetFullPath(baseDir);
                string prefix = root.EndsWith(Path.DirectorySeparatorChar.ToString(),
                    StringComparison.Ordinal) ? root : root + Path.DirectorySeparatorChar;
                relative = full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
                    ? full.Substring(prefix.Length) : Path.GetFileName(full);
            }
            else relative = Path.GetFileName(full);
            relative = relative.Replace(Path.DirectorySeparatorChar, '/');
            if (Path.AltDirectorySeparatorChar != Path.DirectorySeparatorChar)
                relative = relative.Replace(Path.AltDirectorySeparatorChar, '/');
            ValidateWindowsArchiveName(directory ? relative + "/" : relative, directory);
            return relative;
        }

        private static void PreflightCreateItems(List<CreateItem> items, string baseDir)
        {
            var seen = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            foreach (CreateItem item in items)
            {
                string name = MakeArchiveName(item.path, baseDir, item.directory);
                string key = name.Normalize(NormalizationForm.FormC).Replace('/', '\\');
                foreach (var prior in seen)
                {
                    if (string.Equals(prior.Key, key, StringComparison.OrdinalIgnoreCase) ||
                        (!prior.Value && key.StartsWith(prior.Key + "\\",
                            StringComparison.OrdinalIgnoreCase)) ||
                        (!item.directory && prior.Key.StartsWith(key + "\\",
                            StringComparison.OrdinalIgnoreCase)))
                        throw new IOException("Two source paths map to the same Windows archive name: " + name);
                }
                seen.Add(key, item.directory);
            }
        }

        private static byte[] EncodeUtf8Z(string value)
        {
            byte[] bytes = StrictUtf8.GetBytes(value);
            Array.Resize(ref bytes, bytes.Length + 1);
            return bytes;
        }

        private static bool GetSourceMtime(string path, out BpUint64 mtime,
                                           out ushort flags)
        {
            try
            {
                long seconds = new DateTimeOffset(File.GetLastWriteTimeUtc(path))
                    .ToUnixTimeSeconds();
                if (seconds < 0)
                {
                    mtime = BpUint64.FromUlong(0);
                    flags = 0;
                    return false;
                }
                mtime = BpUint64.FromUlong((ulong)seconds);
                flags = NativeMethods.BP_ENTRY_FLAG_MTIME_VALID;
                return true;
            }
            catch
            {
                mtime = BpUint64.FromUlong(0);
                flags = 0;
                return false;
            }
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

        private static BpArchiveInfo NewArchiveInfo()
        {
            BpArchiveInfo info = new BpArchiveInfo();
            info.method_counts = new uint[8];
            return info;
        }

        private static string FormatTime(BpUint64 mtime, ushort flags)
        {
            if ((flags & NativeMethods.BP_ENTRY_FLAG_MTIME_VALID) == 0 ||
                mtime.ToUlong() > (ulong)long.MaxValue) return "-";
            try
            {
                var dt = DateTimeOffset.FromUnixTimeSeconds((long)mtime.ToUlong()).LocalDateTime;
                return dt.ToString("yyyy-MM-dd HH:mm");
            }
            catch
            {
                return "-";
            }
        }
    }
}
