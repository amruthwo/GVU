#include "resume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef GVU_TEST_ROOTS
#  define RESUME_FILE  "/tmp/gvu_test/resume.dat"
#  define HISTORY_FILE "/tmp/gvu_test/history.dat"
#else
#  define RESUME_FILE  "/mnt/SDCARD/Saves/CurrentProfile/states/PixelReader/resume.dat"
#  define HISTORY_FILE "/mnt/SDCARD/Saves/CurrentProfile/states/PixelReader/history.dat"
#endif

#define MAX_ENTRIES  512
#define HISTORY_MAX   64
/* path (1023) + \t + pos + \t + dur + \n */
#define MAX_LINE     1120

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void mkdir_p(const char *path) {
    char tmp[MAX_LINE];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void ensure_dir(const char *file) {
    char dir[MAX_LINE];
    snprintf(dir, sizeof(dir), "%s", file);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir_p(dir); }
}

/* Parse one line from resume.dat into a ResumeEntry.
   Returns 1 on success, 0 if the line is malformed. */
static int parse_entry(char *line, ResumeEntry *out) {
    /* First tab: separates path from the rest */
    char *tab1 = strchr(line, '\t');
    if (!tab1) return 0;
    *tab1 = '\0';
    char *rest = tab1 + 1;

    /* Optional second tab: separates position from duration */
    char *tab2 = strchr(rest, '\t');
    double duration = 0.0;
    if (tab2) {
        *tab2 = '\0';
        duration = atof(tab2 + 1);
    }

    /* Strip trailing newline from position field */
    char *nl = strchr(rest, '\n');
    if (nl) *nl = '\0';

    strncpy(out->path, line, 1023); out->path[1023] = '\0';
    out->position = atof(rest);
    out->duration = duration;
    return 1;
}

/* -------------------------------------------------------------------------
 * resume_load — return saved position for path, or -1.0
 * ---------------------------------------------------------------------- */

double resume_load(const char *path) {
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) return -1.0;

    char line[MAX_LINE];
    double result = -1.0;
    while (fgets(line, sizeof(line), f)) {
        ResumeEntry e;
        if (!parse_entry(line, &e)) continue;
        if (strcmp(e.path, path) == 0) { result = e.position; break; }
    }
    fclose(f);
    return result;
}

/* -------------------------------------------------------------------------
 * resume_save — write/update position; moves entry to end of file
 * ---------------------------------------------------------------------- */

void resume_save(const char *path, double pos_sec, double duration_sec) {
    ResumeEntry *entries = malloc(MAX_ENTRIES * sizeof(ResumeEntry));
    if (!entries) return;

    int count = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (count < MAX_ENTRIES && fgets(line, sizeof(line), f)) {
            ResumeEntry e;
            if (!parse_entry(line, &e)) continue;
            /* Skip the existing entry for this path (will re-add at end) */
            if (strcmp(e.path, path) == 0) continue;
            entries[count++] = e;
        }
        fclose(f);
    }

    /* Append the updated entry at the end (most recently active = last) */
    if (count < MAX_ENTRIES) {
        strncpy(entries[count].path, path, 1023);
        entries[count].path[1023] = '\0';
        entries[count].position = pos_sec;
        entries[count].duration = (duration_sec > 0.0) ? duration_sec : 0.0;
        count++;
    }

    ensure_dir(RESUME_FILE);
    f = fopen(RESUME_FILE, "w");
    if (f) {
        for (int i = 0; i < count; i++)
            fprintf(f, "%s\t%f\t%f\n",
                    entries[i].path, entries[i].position, entries[i].duration);
        fclose(f);
    }
    free(entries);
}

/* -------------------------------------------------------------------------
 * resume_clear — remove entry for path
 * ---------------------------------------------------------------------- */

void resume_clear(const char *path) {
    ResumeEntry *entries = malloc(MAX_ENTRIES * sizeof(ResumeEntry));
    if (!entries) return;

    int count = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) { free(entries); return; }

    char line[MAX_LINE];
    while (count < MAX_ENTRIES && fgets(line, sizeof(line), f)) {
        ResumeEntry e;
        if (!parse_entry(line, &e)) continue;
        if (strcmp(e.path, path) != 0)
            entries[count++] = e;
    }
    fclose(f);

    f = fopen(RESUME_FILE, "w");
    if (f) {
        for (int i = 0; i < count; i++)
            fprintf(f, "%s\t%f\t%f\n",
                    entries[i].path, entries[i].position, entries[i].duration);
        fclose(f);
    }
    free(entries);
}

/* -------------------------------------------------------------------------
 * resume_load_all — load all entries, most-recent first
 * ---------------------------------------------------------------------- */

int resume_load_all(ResumeEntry **out) {
    ResumeEntry *entries = malloc(MAX_ENTRIES * sizeof(ResumeEntry));
    if (!entries) { *out = NULL; return 0; }

    int count = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (count < MAX_ENTRIES && fgets(line, sizeof(line), f)) {
            ResumeEntry e;
            if (parse_entry(line, &e))
                entries[count++] = e;
        }
        fclose(f);
    }

    /* Reverse in-place: last entry in file = most recently used = first result */
    for (int i = 0, j = count - 1; i < j; i++, j--) {
        ResumeEntry tmp = entries[i]; entries[i] = entries[j]; entries[j] = tmp;
    }

    *out = entries;
    return count;
}

/* -------------------------------------------------------------------------
 * resume_record_completed — append path to history.dat (deduped, capped)
 * ---------------------------------------------------------------------- */

void resume_record_completed(const char *path) {
    char (*paths)[1024] = malloc(HISTORY_MAX * 1024);
    if (!paths) return;

    int count = 0;
    FILE *f = fopen(HISTORY_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (count < HISTORY_MAX && fgets(line, sizeof(line), f)) {
            /* Strip newline */
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (line[0] == '\0') continue;
            /* Skip duplicate */
            if (strcmp(line, path) == 0) continue;
            strncpy(paths[count], line, 1023); paths[count][1023] = '\0';
            count++;
        }
        fclose(f);
    }

    /* If at capacity, drop oldest entries to make room */
    if (count >= HISTORY_MAX) {
        /* Shift entries: drop oldest (index 0), keep [1..count-1] */
        memmove(paths[0], paths[1], (size_t)(count - 1) * 1024);
        count--;
    }

    /* Append new entry at end */
    strncpy(paths[count], path, 1023); paths[count][1023] = '\0';
    count++;

    ensure_dir(HISTORY_FILE);
    f = fopen(HISTORY_FILE, "w");
    if (f) {
        for (int i = 0; i < count; i++)
            fprintf(f, "%s\n", paths[i]);
        fclose(f);
    }
    free(paths);
}

/* -------------------------------------------------------------------------
 * resume_load_completed — load recently-completed paths, most-recent first
 * ---------------------------------------------------------------------- */

int resume_load_completed(char (**out)[1024]) {
    char (*paths)[1024] = malloc(HISTORY_MAX * 1024);
    if (!paths) { *out = NULL; return 0; }

    int count = 0;
    FILE *f = fopen(HISTORY_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (count < HISTORY_MAX && fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (line[0] == '\0') continue;
            strncpy(paths[count], line, 1023); paths[count][1023] = '\0';
            count++;
        }
        fclose(f);
    }

    /* Reverse: last line = most recently completed = first result */
    for (int i = 0, j = count - 1; i < j; i++, j--) {
        char tmp[1024];
        memcpy(tmp, paths[i], 1024);
        memcpy(paths[i], paths[j], 1024);
        memcpy(paths[j], tmp, 1024);
    }

    *out = paths;
    return count;
}
