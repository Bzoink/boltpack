/* Inspect or set Amiga native protection, comments, and timestamps. */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    ULONG protection;
    struct DateStamp date;
    if (argc == 2 && !strcmp(argv[1], "stack")) {
        struct Process *process = (struct Process *)FindTask(NULL);
        struct CommandLineInterface *cli = Cli();
        printf("process_stack_bytes=%ld cli_default_stack_longwords=%ld\n",
               process ? (long)process->pr_StackSize : -1L,
               cli ? (long)cli->cli_DefaultStack : -1L);
        return process && cli ? 0 : 9;
    }
    if (argc != 3) return 2;
    lock = Lock((STRPTR)argv[2], ACCESS_READ);
    if (!lock) return 3;
    fib = (struct FileInfoBlock *)AllocVec(sizeof(*fib), MEMF_ANY | MEMF_CLEAR);
    if (!fib) { UnLock(lock); return 4; }
    if (!Examine(lock, fib)) { FreeVec(fib); UnLock(lock); return 5; }
    protection = fib->fib_Protection;
    if (!strcmp(argv[1], "date")) {
        FreeVec(fib); UnLock(lock);
        date.ds_Days = 10000;
        date.ds_Minute = 600;
        date.ds_Tick = 0;
        return SetFileDate((STRPTR)argv[2], &date) ? 0 : 8;
    }
    if (!strcmp(argv[1], "time")) {
        printf("%ld:%ld:%ld\n", (long)fib->fib_Date.ds_Days,
               (long)fib->fib_Date.ds_Minute, (long)fib->fib_Date.ds_Tick);
        FreeVec(fib); UnLock(lock);
        return 0;
    }
    if (!strcmp(argv[1], "set")) {
        FreeVec(fib); UnLock(lock);
        if (!SetComment((STRPTR)argv[2], (STRPTR)"BoltPack archive comment")) return 6;
        if (!SetProtection((STRPTR)argv[2], (LONG)(protection | FIBF_SCRIPT))) return 7;
        return 0;
    }
    printf("%lu:%s\n", (unsigned long)protection, fib->fib_Comment);
    FreeVec(fib); UnLock(lock);
    return 0;
}
