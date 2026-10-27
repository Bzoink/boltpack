/*
 * NativeMethods.cs -- P/Invoke declarations for boltpack_core.dll
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

using System;
using System.Runtime.InteropServices;

namespace BoltPack
{
    /* Mirror of bp_uint64 struct { uint lo; uint hi; } */
    [StructLayout(LayoutKind.Sequential)]
    public struct BpUint64
    {
        public uint lo;
        public uint hi;

        public double ToDouble()
        {
            return (double)lo + (double)hi * 4294967296.0;
        }

        public ulong ToUlong()
        {
            return ((ulong)hi << 32) | lo;
        }

        public static BpUint64 FromUlong(ulong v)
        {
            BpUint64 r;
            r.lo = (uint)(v & 0xFFFFFFFF);
            r.hi = (uint)(v >> 32);
            return r;
        }
    }

    /* Mirror of bp_archive_info_t */
    [StructLayout(LayoutKind.Sequential)]
    public struct BpArchiveInfo
    {
        public ushort version;
        public ushort flags;
        public uint file_count;
        public byte creator_os;
        public byte creator_cpu;
        public byte creator_ver_major;
        public byte creator_ver_minor;
        public BpUint64 archive_size;
        public uint entry_count;
        public uint dir_count;
        public BpUint64 total_original;
        public BpUint64 total_compressed;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
        public uint[] method_counts;
    }

    /* Mirror of bp_io_t */
    [StructLayout(LayoutKind.Sequential)]
    public struct BpIo
    {
        public IntPtr read;
        public IntPtr write;
        public IntPtr seek;
        public IntPtr tell;
        public IntPtr handle;
    }

    /* Mirror of bp_entry_t */
    [StructLayout(LayoutKind.Sequential)]
    public struct BpEntry
    {
        public IntPtr name;          /* char* */
        public ushort name_len;
        public BpUint64 original_size;
        public BpUint64 compressed_size;
        public uint checksum;
        public byte method;
        public byte checksum_type;
        public uint block_size;
        public byte entry_type;
        public byte platform;
        public BpUint64 mtime;
        public ushort flags;
        public ushort header_size;
        public BpUint64 offset;
    }

    /* Mirror of bp_progress_t */
    [StructLayout(LayoutKind.Sequential)]
    public struct BpProgress
    {
        public uint file_index;
        public uint file_count;
        public IntPtr file_name;     /* const char* */
        public BpUint64 file_orig_size;
        public BpUint64 file_bytes_done;
        public BpUint64 file_comp_size;
        public BpUint64 archive_orig_total;
        public BpUint64 archive_comp_total;
        public BpUint64 archive_orig_done;
    }

    /* Callback delegates */
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int BpListDelegate(ref BpEntry entry, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int BpProgressDelegate(ref BpProgress progress, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr BpExtractOpenDelegate(ref BpEntry entry, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void BpExtractDoneDelegate(ref BpEntry entry, IntPtr outIo, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int BpExtractDoneStatusDelegate(ref BpEntry entry,
        IntPtr outIo, int status, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void BpTestDelegate(ref BpEntry entry, int result, IntPtr userData);

    /* bp_write_fn returns C long, which is 32-bit on both Windows ABIs. */
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int BpWriteDelegate(IntPtr handle, IntPtr buffer, uint count);

    static class NativeMethods
    {
        private const string DLL = "boltpack_core.dll";

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_compress_init();

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_info(ref BpIo io, ref BpArchiveInfo info);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_list(ref BpIo io, BpListDelegate fn, IntPtr userData);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_extract_all(ref BpIo io,
            BpExtractOpenDelegate openFn, BpExtractDoneDelegate doneFn,
            BpProgressDelegate progressFn, IntPtr userData);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_extract_all_ex(ref BpIo io,
            BpExtractOpenDelegate openFn, BpExtractDoneStatusDelegate doneFn,
            BpProgressDelegate progressFn, IntPtr userData);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_test_progress(ref BpIo io,
            BpTestDelegate fn, IntPtr userData,
            BpProgressDelegate progressFn, IntPtr progressData);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_create(IntPtr archive, IntPtr io,
            uint blockSize, byte method);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_create_profile(IntPtr archive,
            IntPtr io, int profile);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr bp_dll_archive_alloc();

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern void bp_dll_archive_free(IntPtr archive);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int bp_dll_archive_add_entry(IntPtr archive, string name,
            byte entryType, byte platform, IntPtr fileIo,
            BpUint64 fileSize, BpUint64 mtime);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_add_entry_ex(IntPtr archive,
            [In, MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.U1)] byte[] nameUtf8,
            byte entryType, byte platform, IntPtr fileIo,
            BpUint64 fileSize, BpUint64 mtime, ushort flags);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_add_fat_metadata(IntPtr archive,
            [In, MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.U1)] byte[] nameUtf8,
            byte subjectType, byte valid, byte value);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_archive_finalize(IntPtr archive);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern void bp_dll_archive_close(IntPtr archive);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern void bp_dll_io_from_file(ref BpIo io, IntPtr fileHandle);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern IntPtr bp_dll_fopen(string path, string mode);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl,
            CharSet = CharSet.Unicode, EntryPoint = "bp_dll_fopen_w")]
        public static extern IntPtr bp_dll_fopen_w(string path, string mode);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl,
            CharSet = CharSet.Unicode)]
        public static extern IntPtr bp_dll_fopen_w_exclusive(string path);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern void bp_dll_fclose(IntPtr fp);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int bp_dll_fclose_checked(IntPtr fp);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern void bp_dll_fseek_start(IntPtr fp);

        [DllImport("kernel32.dll", CallingConvention = CallingConvention.Winapi,
            CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "MoveFileExW")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool MoveFileEx(string existingName, string newName,
            uint flags);

        /* Constants matching bp_types.h */
        public const int BP_OK = 0;
        public const int BP_ERR_IO = -1;
        public const int BP_ERR_NOMEM = -2;
        public const int BP_ERR_CHECKSUM = -5;
        public const int BP_ERR_CORRUPT = -6;
        public const int BP_ERR_PARTIAL = -16;
        public const int BP_ERR_ABORTED = -13;
        public const int BP_PROFILE_INTERCHANGE_1 = 1;
        public const byte BP_TYPE_DATA = 0;
        public const byte BP_TYPE_RSRC = 1;
        public const byte BP_TYPE_META = 2;
        public const byte BP_TYPE_SYMLINK = 3;
        public const byte BP_TYPE_DIR = 4;
        public const byte BP_META_PORTABLE = 0xFE;
        public const byte BP_OS_WINDOWS = 0x09;
        public const ushort BP_ENTRY_FLAG_MTIME_VALID = 0x4000;
        public const ushort BP_ENTRY_FLAG_MODE_VALID = 0x8000;
        public const uint MOVEFILE_REPLACE_EXISTING = 0x00000001;
        public const uint MOVEFILE_WRITE_THROUGH = 0x00000008;
        public const byte BP_METHOD_STORED = 0;
        public const byte BP_METHOD_BOLTLITE = 1;
        public const byte BP_METHOD_BOLTLZ = 2;
        public const byte BP_METHOD_FLUFF = 3;

        public static string MethodName(int m)
        {
            switch (m)
            {
                case BP_METHOD_STORED: return "Stored";
                case BP_METHOD_BOLTLITE: return "BoltLite";
                case BP_METHOD_BOLTLZ: return "boltlz";
                case BP_METHOD_FLUFF: return "fluff";
                default: return "?";
            }
        }
    }
}
