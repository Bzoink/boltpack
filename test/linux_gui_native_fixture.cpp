/* Exercise the production FLTK worker/callback paths without a display. */
#define main boltpack_fltk_main
#include "../src/frontend/linux_gui/main.cpp"
#undef main

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    int rc;
    if (strcmp(argv[1], "c") == 0 || strcmp(argv[1], "profile") == 0) {
        ProgressCtx progress = {};
        progress.cancelled.store(false);
        progress.finished.store(false);
        CreateWork work = {};
        work.input_files.push_back(argv[3]);
        work.out_path = argv[2];
        work.pctx = &progress;
        work.method = BP_METHOD_STORED;
        work.use_interchange_profile = strcmp(argv[1], "profile") == 0;
        work.target_profile = TARGET_PROFILE_NONE;
        work.security_attrs = argc > 4 && strcmp(argv[4], "security") == 0;
        create_worker(&work);
        fprintf(stderr, "preservation warnings: %lu\n", work.preservation_warnings);
        rc = work.result;
    } else if (strcmp(argv[1], "x") == 0) {
        FILE *fp = fopen(argv[2], "rb");
        if (!fp) return 2;
        bp_io_t io;
        bp_io_from_file(&io, fp);
        ExtractCtx ctx;
        ctx.dest_dir = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "security") == 0) ctx.security_attrs = true;
            else if (strcmp(argv[i], "flat") == 0) ctx.mode = EXTRACT_FILES_ONLY;
            else ctx.filter.insert(argv[i]);
        }
        rc = extract_run(&io, &ctx, NULL);
        fclose(fp);
    } else return 2;
    fprintf(stderr, "result: %d; stored: %lu; restored: %lu; hardlinks: %lu\n",
            rc, g_lxa.stored, g_lxa.restored, g_lxa.hardlinks);
    return rc == BP_OK ? 0 : 1;
}
