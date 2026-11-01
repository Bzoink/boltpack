#ifndef BP_TARGET_PROFILES_H
#define BP_TARGET_PROFILES_H
#include <string.h>

typedef enum {
    TARGET_PROFILE_NONE = 0,
    TARGET_PROFILE_ATARI_ST = 1,
    TARGET_PROFILE_DOS_CP850_83,
    TARGET_PROFILE_DOS_CP437_83,
    TARGET_PROFILE_DOS_CP850_LFN,
    TARGET_PROFILE_DOS16_CP850_MINI,
    TARGET_PROFILE_WIN16_CP437_MINI,
    TARGET_PROFILE_OS2_16_CP850_MINI,
    TARGET_PROFILE_OS2_FAT_CP850,
    TARGET_PROFILE_AMIGAOS_68K_LATIN1,
    TARGET_PROFILE_MACOS_CLASSIC_HFS,
    TARGET_PROFILE_SINCLAIR_QL_LATIN1,
    TARGET_PROFILE_RISCOS_LATIN1,
    TARGET_PROFILE_X68000_SJIS,
    TARGET_PROFILE_LINUX_POSIX,
    TARGET_PROFILE_POSIX_UTF8,
    TARGET_PROFILE_PLAN9_UTF8,
    TARGET_PROFILE_LINUX_MINI,
    TARGET_PROFILE_HAIKU_BFS,
    TARGET_PROFILE_CPM86_83
} target_profile_t;

static target_profile_t bp_target_profile_from_name(const char *name)
{
    if (strcmp(name, "atari-st") == 0) return TARGET_PROFILE_ATARI_ST;
    if (strcmp(name, "dos-cp850-83") == 0) return TARGET_PROFILE_DOS_CP850_83;
    if (strcmp(name, "dos-cp437-83") == 0) return TARGET_PROFILE_DOS_CP437_83;
    if (strcmp(name, "dos-cp850-lfn-v1") == 0) return TARGET_PROFILE_DOS_CP850_LFN;
    if (strcmp(name, "dos16-cp850-mini") == 0) return TARGET_PROFILE_DOS16_CP850_MINI;
    if (strcmp(name, "win16-cp437-mini") == 0) return TARGET_PROFILE_WIN16_CP437_MINI;
    if (strcmp(name, "os2-16-cp850-mini") == 0) return TARGET_PROFILE_OS2_16_CP850_MINI;
    if (strcmp(name, "os2-fat-cp850") == 0) return TARGET_PROFILE_OS2_FAT_CP850;
    if (strcmp(name, "amigaos-68k-latin1") == 0) return TARGET_PROFILE_AMIGAOS_68K_LATIN1;
    if (strcmp(name, "macos-classic-hfs-v1") == 0) return TARGET_PROFILE_MACOS_CLASSIC_HFS;
    if (strcmp(name, "sinclair-ql-latin1-v1") == 0) return TARGET_PROFILE_SINCLAIR_QL_LATIN1;
    if (strcmp(name, "riscos-latin1-v1") == 0) return TARGET_PROFILE_RISCOS_LATIN1;
    if (strcmp(name, "x68000-sjis-v1") == 0) return TARGET_PROFILE_X68000_SJIS;
    if (strcmp(name, "linux-posix-v1") == 0) return TARGET_PROFILE_LINUX_POSIX;
    if (strcmp(name, "posix-utf8-v1") == 0) return TARGET_PROFILE_POSIX_UTF8;
    if (strcmp(name, "linux-mini-v1") == 0) return TARGET_PROFILE_LINUX_MINI;
    if (strcmp(name, "plan9-utf8-v1") == 0) return TARGET_PROFILE_PLAN9_UTF8;
    if (strcmp(name, "haiku-bfs-v1") == 0) return TARGET_PROFILE_HAIKU_BFS;
    if (strcmp(name, "cpm86-83") == 0) return TARGET_PROFILE_CPM86_83;
    return TARGET_PROFILE_NONE;
}

#endif
