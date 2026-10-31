/* No-replace same-volume rename for the DJGPP DOS frontend. */
#ifndef BP_DOS_RENAME_H
#define BP_DOS_RENAME_H

#ifdef __DJGPP__
#include <dpmi.h>
#include <errno.h>
#include <go32.h>
#include <libc/dosio.h>
#include <string.h>

static int dos_rename_no_replace(const char *old_path, const char *new_path)
{
    __dpmi_regs regs;
    int old_end, new_start, new_end;
    unsigned short transfer_segment;
    int dpmi_rc;

    old_end = _put_path(old_path);
    if (old_end < 0 || (unsigned long)old_end + 2UL >= __tb_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    new_start = old_end + 1;
    new_end = _put_path2(new_path, new_start);
    if (new_end < new_start || (unsigned long)new_end >= __tb_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    transfer_segment = (unsigned short)(__tb >> 4);
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x7156; /* Windows 95 LFN rename refuses existing destinations. */
    regs.x.ds = transfer_segment;
    regs.x.dx = (unsigned short)(__tb & 0x0fUL);
    regs.x.es = transfer_segment;
    regs.x.di = (unsigned short)((__tb & 0x0fUL) + (unsigned long)new_start);
    dpmi_rc = __dpmi_int(0x21, &regs);
    if (dpmi_rc == 0 && (regs.x.flags & 1U) == 0) return 0;
    if (dpmi_rc != 0 || regs.x.ax != 0x7100) {
        errno = dpmi_rc == 0 ? __doserr_to_errno(regs.x.ax) : EIO;
        return -1;
    }

    /* Older DOS has no LFN API. Its native rename also fails if the
       destination exists; long names will be refused by this fallback. */
    old_end = _put_path(old_path);
    if (old_end < 0 || (unsigned long)old_end + 2UL >= __tb_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    new_start = old_end + 1;
    new_end = _put_path2(new_path, new_start);
    if (new_end < new_start || (unsigned long)new_end >= __tb_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x5600;
    regs.x.ds = transfer_segment;
    regs.x.dx = (unsigned short)(__tb & 0x0fUL);
    regs.x.es = transfer_segment;
    regs.x.di = (unsigned short)((__tb & 0x0fUL) + (unsigned long)new_start);
    dpmi_rc = __dpmi_int(0x21, &regs);
    if (dpmi_rc == 0 && (regs.x.flags & 1U) == 0) return 0;
    errno = dpmi_rc == 0 ? __doserr_to_errno(regs.x.ax) : EIO;
    return -1;
}
#else
#include <stdio.h>
#define dos_rename_no_replace(old_path, new_path) rename((old_path), (new_path))
#endif

#endif
