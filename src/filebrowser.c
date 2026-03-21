#include "filebrowser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Supported extensions
 * ---------------------------------------------------------------------- */

const char *VIDEO_EXTENSIONS[] = { ".mp4", ".mkv", ".avi", ".mov", ".webm" };
const int   VIDEO_EXT_COUNT    = 5;

/* -------------------------------------------------------------------------
 * Scan root paths
 * ---------------------------------------------------------------------- */

#ifdef GVU_TEST_ROOTS
static const char *SCAN_ROOTS[] = {
    "/tmp/gvu_test/Media",
    "/tmp/gvu_test/Roms/MEDIA",
};
#else
static const char *SCAN_ROOTS[] = {
    "/mnt/SDCARD/Media",
    "/mnt/SDCARD/Roms/MEDIA",
};
#endif
static const int SCAN_ROOT_COUNT = 2;

/* -------------------------------------------------------------------------
 * Natural sort comparator
 * Numeric runs are compared as integers so S01E09 < S01E10.
 * ---------------------------------------------------------------------- */

static int natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            /* compare the numeric run as an integer */
            char *end_a, *end_b;
            long na = strtol(a, &end_a, 10);
            long nb = strtol(b, &end_b, 10);
            if (na != nb) return (na > nb) ? 1 : -1;
            a = end_a;
            b = end_b;
        } else {
            int ca = tolower((unsigned char)*a);
            int cb = tolower((unsigned char)*b);
            if (ca != cb) return ca - cb;
            a++;
            b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int videofile_cmp(const void *x, const void *y) {
    return natural_cmp(((VideoFile *)x)->name, ((VideoFile *)y)->name);
}

static int mediafolder_cmp(const void *x, const void *y) {
    return natural_cmp(((MediaFolder *)x)->name, ((MediaFolder *)y)->name);
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int has_video_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; i < VIDEO_EXT_COUNT; i++) {
        if (strcasecmp(dot, VIDEO_EXTENSIONS[i]) == 0) return 1;
    }
    return 0;
}

static char *path_join(const char *dir, const char *name) {
    size_t len = strlen(dir) + 1 + strlen(name) + 1;
    char *p = malloc(len);
    if (p) snprintf(p, len, "%s/%s", dir, name);
    return p;
}

static int path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* -------------------------------------------------------------------------
 * MediaFolder / MediaLibrary growth
 * ---------------------------------------------------------------------- */

static void folder_add_file(MediaFolder *f, const char *dir, const char *name) {
    if (f->file_count == f->file_cap) {
        int newcap = f->file_cap ? f->file_cap * 2 : 8;
        f->files   = realloc(f->files, (size_t)newcap * sizeof(VideoFile));
        f->file_cap = newcap;
    }
    VideoFile *vf = &f->files[f->file_count++];
    vf->path = path_join(dir, name);
    vf->name = strdup(name);
}

static void library_add_folder(MediaLibrary *lib, MediaFolder *f) {
    if (lib->folder_count == lib->folder_cap) {
        int newcap  = lib->folder_cap ? lib->folder_cap * 2 : 16;
        lib->folders = realloc(lib->folders, (size_t)newcap * sizeof(MediaFolder));
        lib->folder_cap = newcap;
    }
    lib->folders[lib->folder_count++] = *f;
}

/* -------------------------------------------------------------------------
 * Recursive scan
 *
 * A directory is a "media folder" if it directly contains at least one
 * video file. Sub-directories are always recursed into as well.
 * ---------------------------------------------------------------------- */

static void scan_dir(MediaLibrary *lib, const char *path) {
    DIR *d = opendir(path);
    if (!d) return;

    MediaFolder folder = {0};
    folder.path = strdup(path);
    /* basename: last component after '/' */
    const char *slash = strrchr(path, '/');
    folder.name = strdup(slash ? slash + 1 : path);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* skip hidden + . .. */

        char *child = path_join(path, ent->d_name);
        if (!child) continue;

        if (path_is_dir(child)) {
            scan_dir(lib, child); /* recurse */
        } else if (has_video_ext(ent->d_name)) {
            folder_add_file(&folder, path, ent->d_name);
        }
        free(child);
    }
    closedir(d);

    if (folder.file_count > 0) {
        /* Sort files in natural order */
        qsort(folder.files, (size_t)folder.file_count, sizeof(VideoFile), videofile_cmp);

        /* Cover art lookup */
        char *cjpg = path_join(path, "cover.jpg");
        char *cpng = path_join(path, "cover.png");
        if (cjpg && path_exists(cjpg)) {
            folder.cover = cjpg;
            free(cpng);
        } else if (cpng && path_exists(cpng)) {
            folder.cover = cpng;
            free(cjpg);
        } else {
            free(cjpg);
            free(cpng);
            folder.cover = NULL; /* will use default_cover */
        }

        library_add_folder(lib, &folder);
    } else {
        /* No video files directly here — discard the folder entry */
        free(folder.path);
        free(folder.name);
        free(folder.files);
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void library_scan(MediaLibrary *lib) {
    memset(lib, 0, sizeof(*lib));
    for (int i = 0; i < SCAN_ROOT_COUNT; i++) {
        scan_dir(lib, SCAN_ROOTS[i]);
    }
    /* Sort folders in natural order */
    if (lib->folder_count > 1) {
        qsort(lib->folders, (size_t)lib->folder_count, sizeof(MediaFolder), mediafolder_cmp);
    }
}

void library_free(MediaLibrary *lib) {
    for (int i = 0; i < lib->folder_count; i++) {
        MediaFolder *f = &lib->folders[i];
        for (int j = 0; j < f->file_count; j++) {
            free(f->files[j].path);
            free(f->files[j].name);
        }
        free(f->files);
        free(f->path);
        free(f->name);
        free(f->cover);
    }
    free(lib->folders);
    memset(lib, 0, sizeof(*lib));
}
