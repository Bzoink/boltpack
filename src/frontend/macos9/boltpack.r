/* boltpack.r -- Classic Mac OS PPC GUI resources for BoltPack
 *
 * Compile with CodeWarrior Rez or MPW Rez using the standard Classic Mac
 * RIncludes search path.
 */

#include "Types.r"
#include "SysTypes.r"


/* ============================================================
 * SIZE (-1) -- MultiFinder memory requirements
 * ============================================================ */

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    localAndRemoteHLEvents,
    isStationeryAware,
    useTextEditServices,
    displayManagerAware,
    reserved,
    reserved,
    2 * 1024 * 1024,
    1536 * 1024
};


/* ============================================================
 * vers (1) -- Application version
 * ============================================================ */

resource 'vers' (1) {
    0x01, 0x00, release, 0x00,
    verUS,
    "1.0",
    "BoltPack 1.8 \0xA9 2026 Tony Mattsson"
};


/* ============================================================
 * MBAR + MENUs
 * mApple=128  mFile=129  mEdit=130
 * ============================================================ */

resource 'MBAR' (128) {
    { 128, 129, 130 }
};

resource 'MENU' (128) {
    128,
    textMenuProc,
    allEnabled,
    enabled,
    apple,
    {
        "About BoltPack\0xC9",  noIcon, noKey, noMark, plain;
        "-",                     noIcon, noKey, noMark, plain
    }
};

resource 'MENU' (129) {
    129,
    textMenuProc,
    allEnabled,
    enabled,
    "File",
    {
        "Open Archive\0xC9",    noIcon, "O", noMark, plain;
        "Create Archive\0xC9",  noIcon, "N", noMark, plain;
        "-",                     noIcon, noKey, noMark, plain;
        "Preferences\0xC9",     noIcon, noKey, noMark, plain;
        "-",                     noIcon, noKey, noMark, plain;
        "Quit",                  noIcon, "Q", noMark, plain
    }
};

resource 'MENU' (130) {
    130,
    textMenuProc,
    allEnabled,
    enabled,
    "Edit",
    {
        "Undo",   noIcon, "Z", noMark, plain;
        "-",      noIcon, noKey, noMark, plain;
        "Cut",    noIcon, "X", noMark, plain;
        "Copy",   noIcon, "C", noMark, plain;
        "Paste",  noIcon, "V", noMark, plain;
        "Clear",  noIcon, noKey, noMark, plain
    }
};


/* ============================================================
 * Main window (rMainWindow = 128)
 * ============================================================ */

resource 'WIND' (128, "BoltPack") {
    { 60, 100, 350, 490 },
    documentProc,
    invisible,
    goAway,
    0x0,
    "BoltPack",
    noAutoCenter
};


/* ============================================================
 * Error alert (rAlrtError = 128)
 * ============================================================ */

resource 'DITL' (128) {
    {
        { 74, 166, 94, 244 }, Button { enabled, "OK" };
        {  8,  76, 68, 344 }, StaticText { disabled, "^0" }
    }
};

resource 'ALRT' (128) {
    { 120, 80, 230, 450 },
    128,
    {
        OK, visible, sound1;
        OK, visible, sound1;
        OK, visible, sound1;
        OK, visible, sound1
    },
    alertPositionParentWindow
};


/* ============================================================
 * Progress dialog (rDlgProgress = 128) -> DITL 129
 * ============================================================ */

resource 'DITL' (129) {
    {
        {  10,  10,  26, 369 }, StaticText { enabled,  "" };
        {  32,  10,  48, 369 }, UserItem   { disabled };
        {  57,  10,  73, 189 }, StaticText { enabled,  "" };
        {  57, 195,  73, 369 }, StaticText { enabled,  "" };
        { 160, 139, 180, 239 }, Button     { enabled,  "Cancel" };
        {  80,  10,  96, 139 }, StaticText { disabled, "Overall:" };
        { 100,  10, 116, 369 }, UserItem   { disabled }
    }
};

resource 'DLOG' (128) {
    { 40, 40, 260, 430 },
    movableDBoxProc,
    invisible,
    noGoAway,
    0x0,
    129,
    "",
    centerParentWindow
};


/* ============================================================
 * Mixed-action dialog (rDlgMixed = 129) -> DITL 130
 * ============================================================ */

resource 'DITL' (130) {
    {
        {  74,  20,  94, 120 }, Button     { enabled, "Unpack" };
        {  74, 140,  94, 240 }, Button     { enabled, "Pack" };
        {  74, 260,  94, 360 }, Button     { enabled, "Cancel" };
        {   8,  20,  58, 360 }, StaticText { disabled,
            "Dropped items contain both archives and regular files.\
Choose what BoltPack should do." }
    }
};

resource 'DLOG' (129) {
    { 140, 60, 260, 450 },
    movableDBoxProc,
    invisible,
    noGoAway,
    0x0,
    130,
    "Choose Action",
    centerParentWindow
};


/* ============================================================
 * Archive-name dialog (rDlgName = 130) -> DITL 131
 * ============================================================ */

resource 'DITL' (131) {
    {
        { 66,  20, 86, 120 }, Button   { enabled, "OK" };
        { 34,  20, 52, 360 }, EditText { enabled, "" };
        { 66, 240, 86, 360 }, Button   { enabled, "Cancel" };
        {  8,  20, 26, 360 }, StaticText { disabled, "Archive name:" }
    }
};

resource 'DLOG' (130) {
    { 160, 50, 266, 430 },
    movableDBoxProc,
    invisible,
    noGoAway,
    0x0,
    131,
    "Create Archive",
    centerParentWindow
};


/* ============================================================
 * Preferences dialog (rDlgPrefs = 131) -> DITL 132
 * ============================================================ */

resource 'DITL' (132) {
    {
        {  20,  20,  38, 280 }, CheckBox   { enabled,
            "Ask before choosing destination folder" };
        {  70,  40,  90, 140 }, Button     { enabled, "OK" };
        {  70, 180,  90, 280 }, Button     { enabled, "Cancel" };
        { 100,  20, 132, 320 }, StaticText { disabled,
            "Requires Navigation Services. When unavailable, extracted files\
always go beside the archive." }
    }
};

resource 'DLOG' (131) {
    { 150, 70, 300, 410 },
    movableDBoxProc,
    invisible,
    noGoAway,
    0x0,
    132,
    "Preferences",
    centerParentWindow
};


/* ============================================================
 * About dialog (rDlgAbout = 132) -> DITL 133
 * ============================================================ */

resource 'DITL' (133) {
    {
        {  86, 136, 106, 216 }, Button     { enabled,  "OK" };
        {   8,  20,  28, 332 }, StaticText { disabled, "BoltPack 1.8" };
        {  32,  20,  52, 332 }, StaticText { disabled,
            "\0xA9 2026 Tony Mattsson" };
        {  56,  20,  76, 332 }, StaticText { disabled,
            "A lightweight file archiver." }
    }
};

resource 'DLOG' (132) {
    { 160, 80, 280, 432 },
    movableDBoxProc,
    invisible,
    noGoAway,
    0x0,
    133,
    "About BoltPack",
    centerParentWindow
};


/* ============================================================
 * STR# resources
 * ============================================================ */

resource 'STR#' (128) {
    {
        "(unused)";
        "I/O error";
        "Out of memory";
        "Invalid archive format";
        "Unsupported archive version";
        "Checksum mismatch \0x97 archive may be corrupt";
        "Corrupt data block";
        "Archive is truncated";
        "Arithmetic overflow";
        "File name too long";
        "Unsupported compression method";
        "Compression failed";
        "Decompression failed";
        "Operation aborted";
        "File not found";
        "Invalid argument";
        "Unknown error"
    }
};

resource 'STR#' (129) {
    {
        "Drop files here to pack or unpack";
        "Expand";
        "Compress";
        "Ask for destination:"
    }
};


/* ============================================================
 * PICT 128 -- Application logo (rLogoPict)
 * ============================================================ */

data 'PICT' (128) {
	$"28D80000000000170017001102FF0C00FFFE0000012C0000012C000000000000"
	$"0060006000000000001E0001000A800180017FFF7FFF009B000000FF81800000"
	$"0000006000600000000400000000004800000048000000100020000300080000"
	$"0000079528A400000000000000000060006000000000006000600040000A0000"
	$"000000600060000681FF81FFE1FF000681FF81FFE1FF000681FF81FFE1FF0006"
	$"81FF81FFE1FF000681FF81FFE1FF000681FF81FFE1FF000681FF81FFE1FF0006"
	$"81FF81FFE1FF000681FF81FFE1FF000681FF81FFE1FF001AE0FF00D4EAC100CE"
	$"BAFF00D4EAC100CEBAFF00D4EAC100CEDBFF001DE1FF01EC22EA0000DABBFF01"
	$"EC22EA0000DABBFF01EC22EA0000DADBFF001AE1FF0092EA0000ACBAFF0092EA"
	$"0000ACBAFF0092EA0000ACDAFF0035E2FF00DEFD000016FE490022F300016EFB"
	$"BBFF00DEFD000016FE490022F300016EFBBBFF00DEFD000016FE490022F30001"
	$"6EFBDAFF0047E3FF05FE7700002AEFFDFF008CF4000626E7FFFFE8EFF8C0FF05"
	$"FE7700002AEFFDFF008CF4000626E7FFFFE8EFF8C0FF05FE7700002AEFFDFF00"
	$"8CF4000626E7FFFFE8EFF8DEFF006EE3FF00CEFE0000B2FDFF00DCF30003BFFF"
	$"FFD0FE000D164D6A8092A0AEBBC7D2DDE8F1FBCEFF00CEFE0000B2FDFF00DCF3"
	$"0003BFFFFFD0FE000D164D6A8092A0AEBBC7D2DDE8F1FBCEFF00CEFE0000B2FD"
	$"FF00DCF30003BFFFFFD0FE000D164D6A8092A0AEBBC7D2DDE8F1FBECFF0065E4"
	$"FF05FA5A00004DF7FEFF01FD71F4000486FEFFF349F500072E56718595AFD5F8"
	$"D2FF05FA5A00004DF7FEFF01FD71F4000486FEFFF349F500072E56718595AFD5"
	$"F8D2FF05FA5A00004DF7FEFF01FD71F4000486FEFFF349F500072E56718595AF"
	$"D5F8EFFF0065E4FF00BDFE0000C7FDFF00C8F4001142F0FFFFB97C7F909FACB9"
	$"C4CED7E0EAF2FACAFF00BDFE0000C7FDFF00C8F4001142F0FFFFB97C7F909FAC"
	$"B9C4CED7E0EAF2FACAFF00BDFE0000C7FDFF00C8F4001142F0FFFFB97C7F909F"
	$"ACB9C4CED7E0EAF2FAE7FF0038E5FF05F540000068FCFEFF01F74FF40000D0BA"
	$"FF05F540000068FCFEFF01F74FF40000D0BAFF05F540000068FCFEFF01F74FF4"
	$"0000D0D6FF0032E5FF00A7FE0000D8FDFF00B1F400009AB9FF00A7FE0000D8FD"
	$"FF00B1F400009AB9FF00A7FE0000D8FDFF00B1F400009AD5FF0068E6FF04EA1C"
	$"000088FDFF01EE2AF500115DF7FFFAADAAB4B6BFC1CACDD5DEE5ECF3F9CAFF04"
	$"EA1C000088FDFF01EE2AF500115DF7FFFAADAAB4B6BFC1CACDD5DEE5ECF3F9CA"
	$"FF04EA1C000088FDFF01EE2AF500115DF7FFFAADAAB4B6BFC1CACDD5DEE5ECF3"
	$"F9E5FF007AE6FF048F000016E7FDFF0096F5000416DFFFFFAAF2001049627584"
	$"929DAAB4BEC7CFD6DEE7EDF3F8DCFF048F000016E7FDFF0096F5000416DFFFFF"
	$"AAF2001049627584929DAAB4BEC7CFD6DEE7EDF3F8DCFF048F000016E7FDFF00"
	$"96F5000416DFFFFFAAF2001049627584929DAAB4BEC7CFD6DEE7EDF3F8F7FF00"
	$"79E6FF00C8FE0000CDFDFF00C6F4000D2BE3FFFFA997A6B3BEC9D2DAE2EAF2F9"
	$"D4FF00C8FE0000CDFDFF00C6F4000D2BE3FFFFA997A6B3BEC9D2DAE2EAF2F9D4"
	$"FF00C8FE0000CDFDFF00C6F4000D2BE3FFFFA997A6B3BEC9D2DAE2EAF2F9F3FF"
	$"0055E7FF054800004DF7FEFF015ADEF30000F1BAFF054800004DF7FEFF015ADE"
	$"F30000F1BAFF054800004DF7FEFF015ADEF30000F1D3FF0044E7FF00A1FE0000"
	$"D9FDFF00ACF4000087B9FF00A1FE0000D9FDFF00ACF4000087B9FF00A1FE0000"
	$"D9FDFF00ACF4000087D1FF007EE8FF04E201000074FDFF01E708F5000B56F5FF"
	$"FFBAC2C8CDD3D9DFE5F9C6FF04E201000074FDFF01E708F5000B56F5FFFFBAC2"
	$"C8CDD3D9DFE5F9C6FF04E201000074FDFF01E708F5000B56F5FFFFBAC2C8CDD3"
	$"D9DFE5F9E0FF006DE8FF0055FE0000E0FDFF005EF50005BDF6FFFFFD7CF30001"
	$"8DF7FEFF0063FE0001F7FDFFF2FF0055FE0000E0FDFF005EF50005BDF6FFFFFD"
	$"7CF300018DF7FEFF0063FE0001F7FDFFF2FF0055FE0000E0FDFF005EF50005BD"
	$"F6FFFFFD7CF300018DF7FEFF0063FE0001F7FDFFFFDF00CCE8FF00B6FE0000C7"
	$"FDFF00C0F4001146EAFFFFC0909EABB6C0C9D1D9E1E8EEF4FAC7FF00B6FE0000"
	$"C7FDFF00C0F4001146EAFFFFC0909EABB6C0C9D1D9E1E8EEF4FAC7FF00B6FE00"
	$"00C7FDFF00C0F4001146EAFFFFC0909EABB6C0C9D1D9E1E8EEF4FAE2FF001CE9"
	$"FF00ECFE000016FE230053EF0000BEBBFF00ECFE000016FE230053EF0000BEBB"
	$"FF00ECFE000016FE230053EF0000BED3FF0003E9FF05B5000026E7FDFF01BF00"
	$"F4000374F8FFFFE8FF05B5000026E7FDFF01BF00F4000374F8FFFFE8FF05B500"
	$"0026E7FDFF01BF00F4000374F8FFFFF8FF0000E9FF00A0FE0000D9FDFF00AAF4"
	$"00009BB9FF00A0FE0000D9FDFF00AAF400009BB9FF00A0FE0000D9FDFF00AAF4"
	$"00009BCFFF0036EAFF05DC0B000068FCFEFF01E311F4001448F0FFFFA39FA6AB"
	$"B1B7BCC4CAD1D7DEE4EAF0F7D9FF05DC0B000068FCFEFF01E311F4001448F0FF"
	$"FFA39FA6ABB1B7BCC4CAD1D7DEE4EAF0F7D9FF05DC0B000068FCFEFF01E311F4"
	$"001448F0FFFFA39FA6ABB1B7BCC4CAD1D7DEE4EAF0F7F3FF0029EAFF057E0000"
	$"16E7FDFF018700F50000BDBAFF057E000016E7FDFF018700F50000BDBAFF057E"
	$"000016E7FDFF018700F50000BDE4FF0020EAFF00D8FE0000B2FDFF00E4F30004"
	$"76FDFFFFC5F30003EBF6FFFFDDFF00D8FE0000B2FDFF00E4F3000476FDFFFFC5"
	$"F30003EBF6FFFFDDFF00D8FE0000B2FDFF00E4F3000476FDFFFFC5F30003EBF6"
	$"FFFFFAFF0053EBFF00A8FE0000DBFDFF00B1F4000B79FBFFFF91FBFFF6E5EAF6"
	$"F6D4FF00A8FE0000DBFDFF00B1F4000B79FBFFFF91FBFFF6E5EAF6F6D4FF00A8"
	$"FE0000DBFDFF00B1F4000B79FBFFFF91FBFFF6E5EAF6F6F3FF005CEBFF0036FE"
	$"0000E8FDFF003CF50013E5F7FFFF5F818F96A0AAB3BCC5CFD8E0E8F0F7E9FF00"
	$"36FE0000E8FDFF003CF50013E5F7FFFF5F818F96A0AAB3BCC5CFD8E0E8F0F7E9"
	$"FF0036FE0000E8FDFF003CF50013E5F7FFFF5F818F96A0AAB3BCC5CFD8E0E8F0"
	$"F7FFFAFF0086EBFF03F1020000DAFDFF018DFAF3FF008DBAFF03F1020000DAFD"
	$"FF018DFAF3FF008DBAFF03F1020000DAFDFF018DFAF3FF008DE2FF002FEBFF00"
	$"A6FE0000DDFDFF00ADF40017A2FDFFDEB8C4CED5DBE1E7ECF1F6FAFCFDFEFFFF"
	$"FFFFFFF4FF00A6FE0000DDFDFF00ADF40017A2FDFFDEB8C4CED5DBE1E7ECF1F6"
	$"FAFCFDFEFFFFFFFFF4FF00A6FE0000DDFDFF00ADF40017A2FDFFDEB8C4CED5DB"
	$"E1E7ECF1F6FAFCFDFEFFFFFFFFFFF8FF0076EBFF0058FE0000E7FDFF0060F500"
	$"13EAF9FFFFD8CDD3D9DFE4E9EEF2F6FAFCFDFDFEF8FF0058FE0000E7FDFF0060"
	$"F50013EAF9FFFFD8CDD3D9DFE4E9EEF2F6FAFCFDFDFEF8FF0058FE0000E7FDFF"
	$"0060F50013EAF9FFFFD8CDD3D9DFE4E9EEF2F6FAFCFDFDFFFFE3FF00C0ECFF00"
	$"A3FE0000E0FDFF00AFF4000D8AFCFFFFA6FBFFF8ECEFF8F7D9FF00A3FE0000E0"
	$"FDFF00AFF4000D8AFCFFFFA6FBFFF8ECEFF8F7D9FF00A3FE0000E0FDFF00AFF4"
	$"000D8AFCFFFFA6FBFFF8ECEFF8F7F7FF00BFECFF001CFE0000F0FDFF0024F500"
	$"00F8D5FF001CFE0000F0FDFF0024F50000F8D5FF001CFE0000F0FDFF0024F500"
	$"00F8E8FF0000ECFF03F43A000074FCFEFF015730F40000D0AFFF03F43A000074"
	$"FCFEFF015730F40000D0AFFF03F43A000074FCFEFF015730F40000D0C1FF0000"
	$"ECFF0083FE0000D9FDFF008AF4000B7CFCFFFF8CFBFFF7E6EBF7F6D2FF0083FE"
	$"0000D9FDFF008AF4000B7CFCFFFF8CFBFFF7E6EBF7F6D2FF0083FE0000D9FDFF"
	$"008AF4000B7CFCFFFF8CFBFFF7E6EBF7F6F1FF0000ECFF0015FE0000F6FDFF00"
	$"1AF500049CF6FFFFE3F5000013B4FF0015FE0000F6FDFF001AF500049CF6FFFF"
	$"E3F5000013B4FF0015FE0000F6FDFF001AF500049CF6FFFFE3F5000013C6FF00"
	$"00ECFF0000FE0000FCFDFF0000F5000000C0FF0000FE0000FCFDFF0000F50000"
	$"00C0FF0000FE0000FCFDFF0000F5000000D5FF0000ECFF0000FE0000FEFDFF00"
	$"00F5000000C6FF0000FE0000FEFDFF0000F5000000C6FF0000FE0000FEFDFF00"
	$"00F5000000E2FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"
	$"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"
};
