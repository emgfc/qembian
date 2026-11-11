/* Lossless transport journal. Included after syborg_gl2.h. Not a driver-state snapshot.
 * File: SYGLCAP1, then LE {owner, command_words}, followed by the original command.
 * Synthetic op 0xfffffff0 marks capture start; 0xfffffff1 marks each target present.
 * Journalling must start before the first command; a failed/oversize journal is never captured.
 */
#define GL_CAPTURE_LIMIT (2ULL << 30)
static FILE *gl_journal, *gl_capture;
static bool gl_journal_init, gl_journal_bad;
static uint64_t gl_journal_bytes;
static uint32_t gl_last_owner, gl_last_surf, gl_capture_owner, gl_capture_surf;
static unsigned gl_capture_frames;
static char *gl_capture_path;

static bool gl_capture_write(FILE *f, uint32_t owner, const uint32_t *cmd, size_t words)
{
    uint32_t h[2] = { cpu_to_le32(owner), cpu_to_le32(words) };
    if (fwrite(h, sizeof(h), 1, f) != 1) return false;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    return fwrite(cmd, 4, words, f) == words;
#else
    for (size_t i = 0; i < words; i++) {
        uint32_t w = cpu_to_le32(cmd[i]);
        if (fwrite(&w, 4, 1, f) != 1) return false;
    }
    return true;
#endif
}

static void gl_capture_record(SyborgGlState *s, const uint32_t *cmd, size_t words)
{
    if (!gl_journal_init) {
        const char *path = getenv("SYBORG_GL_JOURNAL");
        gl_journal_init = true;
        if (path && *path) {
            gl_journal = fopen(path, "w+b");
            if (!gl_journal || fwrite("SYGLCAP1", 8, 1, gl_journal) != 1) gl_journal_bad = true;
            gl_journal_bytes = 8;
        }
    }
    if (cmd[0] == SYGL_GL2 && words >= 4 &&
        (cmd[3] == GL2_FN_glDrawElements || cmd[3] == GL2_FN_glDrawArrays)) {
        Gl2HostCtx *c = gl2_find(s, cmd[2]);
        if (c) { gl_last_owner = s->owner; gl_last_surf = c->surf; }
    }
    if (gl_journal && !gl_journal_bad) {
        gl_journal_bytes += 8 + words * 4;
        if (gl_journal_bytes > GL_CAPTURE_LIMIT || !gl_capture_write(gl_journal, s->owner, cmd, words)) {
            gl_journal_bad = true;
            warn_report("syborg-gl: capture journal failed or exceeded 2 GiB; restart to capture");
        }
    }
    if (!gl_capture) return;
    bool ok = !gl_journal_bad && gl_capture_write(gl_capture, s->owner, cmd, words);
    if (ok && cmd[0] == SYGL_VG_PRESENT && words >= 7 &&
        s->owner == gl_capture_owner && cmd[2] == gl_capture_surf) {
        uint32_t marker[] = { 0xfffffff1, 3, ++gl_capture_frames };
        ok = gl_capture_write(gl_capture, s->owner, marker, 3);
    }
    if (!ok || gl_capture_frames == 31) {
        /* First presentation finishes the partial frame at the trigger; then 30 complete frames. */
        if (fclose(gl_capture)) ok = false;
        gl_capture = NULL;
        if (!ok) { gl_capture_frames = 0; warn_report("syborg-gl: capture write failed"); }
    }
}

static char *gl_capture_status(Object *o, Error **errp)
{
    return g_strdup_printf("%s:%u:%s", gl_capture ? "recording" :
                           gl_capture_frames == 31 ? "complete" : "idle",
                           gl_capture_frames > 0 ? gl_capture_frames - 1 : 0,
                           gl_capture_path ? gl_capture_path : "");
}

static void gl_capture_start(Object *o, const char *path, Error **errp)
{
    if (gl_capture || !gl_journal || gl_journal_bad || !gl_last_surf) {
        error_setg(errp, "Capture needs a fresh journal-enabled boot and a drawing GLES app; another capture may be active");
        return;
    }
    FILE *out = fopen(path, "wbx");
    if (!out) { error_setg_errno(errp, errno, "Cannot create capture"); return; }
    bool ok = !fflush(gl_journal) && !fseeko(gl_journal, 0, SEEK_SET);
    uint8_t buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), gl_journal))) {
        ok = fwrite(buf, 1, n, out) == n;
    }
    ok = ok && !ferror(gl_journal);
    if (fseeko(gl_journal, 0, SEEK_END)) { gl_journal_bad = true; ok = false; }
    uint32_t marker[] = { 0xfffffff0, 4, gl_last_owner, gl_last_surf };
    ok = ok && gl_capture_write(out, gl_last_owner, marker, 4);
    if (!ok) {
        fclose(out);
        error_setg(errp, "Could not copy GPU history; incomplete capture retained");
        return;
    }
    g_free(gl_capture_path);
    gl_capture_path = g_strdup(path);
    gl_capture_owner = gl_last_owner;
    gl_capture_surf = gl_last_surf;
    gl_capture_frames = 0;
    gl_capture = out;
}
