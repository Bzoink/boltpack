/*
 * MacCompat.h -- Classic Mac OS Toolbox declarations missing from Retro68's
 *                Multiverse.h subset.  Force-included via -include in the
 *                Makefile; do not #include this file directly.
 *
 * Only APIs that are genuinely absent from Multiverse.h are declared here.
 * Trap constants follow Apple Universal Interfaces 3.4.1 / Inside Macintosh.
 */

#ifndef MACCOMPAT_H
#define MACCOMPAT_H

#include <Multiverse.h>

/* =========================================================================
 * Gestalt selectors missing from Multiversal Interfaces
 * ======================================================================= */

#ifndef gestaltAppearanceVersion
#define gestaltAppearanceVersion 'appr'
#endif

#ifndef gestaltDragMgrPresent
#define gestaltDragMgrPresent  'drag'
#endif

/* =========================================================================
 * Appearance Manager
 * RegisterAppearanceClient: TWOWORDINLINE(0x7001, 0xAA74)
 * ======================================================================= */

pascal OSErr RegisterAppearanceClient(void)
    M68K_INLINE(0x7001, 0xAA74);

/* =========================================================================
 * Folders Manager -- missing pieces only
 * kPreferencesFolderType is already in Multiverse.h.
 * kOnSystemDisk, kCreateFolder, and DirCreate are absent.
 * DirCreate: HFSDispatch $A260, selector 6.
 * ======================================================================= */

enum {
    kOnSystemDisk     = (short)0x8000,  /* = -32768 */
    kCreateFolder     = 1,
    kDontCreateFolder = 0
};

pascal OSErr DirCreate(short vRefNum, long parentDirID,
    ConstStr255Param directoryName, long *createdDirID)
    M68K_INLINE(0x7006, 0xA260);

/* =========================================================================
 * Dialog Manager -- GetDialogWindow
 * In pre-Carbon 68k, DialogPtr IS a WindowPtr, so this is a safe cast.
 * ======================================================================= */

#ifndef GetDialogWindow
#define GetDialogWindow(dlg) ((WindowPtr)(dlg))
#endif

/* =========================================================================
 * Drag Manager
 * All calls go through trap $ABED (DragDispatch).
 * ======================================================================= */

typedef unsigned long  DragReference;
typedef short          DragTrackingMessage;
typedef unsigned long  ItemReference;
typedef OSType         FlavorType;

enum {
    flavorTypeHFS = 'hfs '
};

enum {
    kDragTrackingEnterHandler = 1,
    kDragTrackingEnterWindow  = 2,
    kDragTrackingInWindow     = 3,
    kDragTrackingLeaveWindow  = 4,
    kDragTrackingLeaveHandler = 5
};

typedef struct HFSFlavor {
    OSType          fileType;
    OSType          fileCreator;
    unsigned short  fdFlags;
    FSSpec          fileSpec;
} HFSFlavor;

typedef pascal OSErr (*DragTrackingHandlerUPP)(DragTrackingMessage message,
    WindowPtr theWindow, void *handlerRefCon, DragReference theDrag);

typedef pascal OSErr (*DragReceiveHandlerUPP)(WindowPtr theWindow,
    void *handlerRefCon, DragReference theDrag);

/* InstallTrackingHandler: TWOWORDINLINE(0x7001, 0xABED) */
pascal OSErr InstallTrackingHandler(DragTrackingHandlerUPP trackingHandler,
    WindowPtr theWindow, void *handlerRefCon)
    M68K_INLINE(0x7001, 0xABED);

/* InstallReceiveHandler: TWOWORDINLINE(0x7002, 0xABED) */
pascal OSErr InstallReceiveHandler(DragReceiveHandlerUPP receiveHandler,
    WindowPtr theWindow, void *handlerRefCon)
    M68K_INLINE(0x7002, 0xABED);

/* CountDragItems: TWOWORDINLINE(0x700E, 0xABED) */
pascal OSErr CountDragItems(DragReference theDrag, unsigned short *numItems)
    M68K_INLINE(0x700E, 0xABED);

/* GetDragItemReferenceNumber: TWOWORDINLINE(0x700F, 0xABED) */
pascal OSErr GetDragItemReferenceNumber(DragReference theDrag,
    unsigned short index, ItemReference *theItemRef)
    M68K_INLINE(0x700F, 0xABED);

/* GetFlavorData: TWOWORDINLINE(0x7014, 0xABED) */
pascal OSErr GetFlavorData(DragReference theDrag, ItemReference theItemRef,
    FlavorType theType, void *dataPtr, Size *dataSize, unsigned long dataOffset)
    M68K_INLINE(0x7014, 0xABED);

/* ShowDragHilite: TWOWORDINLINE(0x701E, 0xABED) */
pascal OSErr ShowDragHilite(DragReference theDrag, RgnHandle hiliteFrame,
    Boolean inside)
    M68K_INLINE(0x701E, 0xABED);

/* HideDragHilite: TWOWORDINLINE(0x701F, 0xABED) */
pascal OSErr HideDragHilite(DragReference theDrag)
    M68K_INLINE(0x701F, 0xABED);

#endif /* MACCOMPAT_H */

