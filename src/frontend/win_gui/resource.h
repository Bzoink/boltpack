/*
 * resource.h -- resource IDs for BoltPack Win32 GUI
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_RESOURCE_H
#define BP_RESOURCE_H

/* Menu */
#define IDR_MAINMENU            100
#define IDR_ACCEL               101

#define IDM_OPEN                1001
#define IDM_EXTRACT             1002
#define IDM_CLOSE               1003
#define IDM_VALIDATE            1008
#define IDM_INFO                1009
#define IDM_CREATE              1004
#define IDM_EXIT                1005
#define IDM_DOC                 1006
#define IDM_ABOUT               1007

/* Dialogs */
#define IDD_ABOUT               200
#define IDD_PROGRESS            201
#define IDD_DOC                 202

/* Controls (progress dialog) */
#define IDC_PROG_FILE           2001
#define IDC_PROG_COUNT          2002
#define IDC_PROG_BAR            2003
#define IDC_PROG_CANCEL         2004

/* Controls (about dialog) */
#define IDC_ABOUT_NAME          2010
#define IDC_ABOUT_DESC          2011
#define IDC_ABOUT_COPYRIGHT     2012
#define IDC_ABOUT_LICENSE       2013

/* Controls (documentation dialog) */
#define IDC_DOC_TEXT            2020

/* Icon */
#define IDI_BOLTPACK            300

/* Logo bitmap */
#define IDB_LOGO                301

#endif /* BP_RESOURCE_H */
