/* Verified sibling-stage publication policy, isolated for host fault tests. */
#ifndef BP_OS2_PUBLISH_H
#define BP_OS2_PUBLISH_H

#include "../../core/bp_types.h"
#include <stdio.h>
#include <string.h>

#define OS2_PUBLISH_PATH_MAX 260U

typedef struct {
    int (*query)(void *context, const char *path, int *is_directory);
    int (*move)(void *context, const char *from, const char *to);
    int (*remove)(void *context, const char *path);
} os2_publish_ops_t;

/* query returns 1 for an existing path, 0 when absent, and -1 on error. */
static int os2_publish_staged_file(const char *temporary, const char *target,
                                   int force, char *recovery,
                                   size_t recovery_size,
                                   unsigned long *sequence,
                                   const os2_publish_ops_t *ops,
                                   void *context, int *committed)
{
    int exists, is_directory = 0, has_backup = 0;
    unsigned int attempt;
    char backup[OS2_PUBLISH_PATH_MAX];

    if (temporary == NULL || target == NULL || recovery == NULL ||
        recovery_size == 0 || sequence == NULL || ops == NULL ||
        ops->query == NULL || ops->move == NULL || ops->remove == NULL ||
        committed == NULL) return BP_ERR_INVALID_ARG;
    recovery[0] = '\0';
    *committed = 0;

    exists = ops->query(context, target, &is_directory);
    if (exists < 0) return BP_ERR_IO;
    if (exists && is_directory) return BP_ERR_PARTIAL;
    if (exists && !force) return BP_ERR_PARTIAL;

    if (exists) {
        const char *slash = strrchr(target, '\\');
        const char *forward = strrchr(target, '/');
        size_t prefix;
        if (forward != NULL && (slash == NULL || forward > slash))
            slash = forward;
        prefix = slash != NULL ? (size_t)(slash - target + 1) : 0;
        if (prefix + 13U >= sizeof(backup)) return BP_ERR_NAME_TOO_LONG;

        for (attempt = 0; attempt < 128U; attempt++) {
            int backup_exists, unused_directory = 0;
            memcpy(backup, target, prefix);
            sprintf(backup + prefix, "BO%06lX.BAK", (*sequence)++ & 0xFFFFFFUL);
            if (strlen(backup) + 1U > recovery_size)
                return BP_ERR_NAME_TOO_LONG;
            backup_exists = ops->query(context, backup, &unused_directory);
            if (backup_exists < 0) return BP_ERR_IO;
            if (backup_exists) continue;
            if (ops->move(context, target, backup) != 0) return BP_ERR_IO;
            has_backup = 1;
            break;
        }
        if (!has_backup) return BP_ERR_IO;
    }

    if (ops->move(context, temporary, target) != 0) {
        if (has_backup) {
            int target_exists, unused_directory = 0;
            target_exists = ops->query(context, target, &unused_directory);
            if (target_exists != 0 || ops->move(context, backup, target) != 0)
                strcpy(recovery, backup);
        }
        return BP_ERR_IO;
    }

    *committed = 1;
    if (has_backup && ops->remove(context, backup) != 0) {
        if (strlen(backup) + 1U <= recovery_size) strcpy(recovery, backup);
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

#endif
