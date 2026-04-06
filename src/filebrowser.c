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
 * ---------------------------------------------------------------------- */

static int natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
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
            a++; b++;
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
static int season_cmp(const void *x, const void *y) {
    return natural_cmp(((Season *)x)->name, ((Season *)y)->name);
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

/* Returns 1 if path contains at least one video file directly (not recursed) */
static int has_direct_video_files(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL && !found) {
        if (ent->d_name[0] == '.') continue;
        char *child = path_join(path, ent->d_name);
        if (child) {
            if (!path_is_dir(child) && has_video_ext(ent->d_name))
                found = 1;
            free(child);
        }
    }
    closedir(d);
    return found;
}

/* -------------------------------------------------------------------------
 * Movie detection heuristics
 *
 * A file is episode-like if its name contains an SxxExx or NxNN pattern.
 * A folder is movie-like if it has exactly 1 video file and no episode names.
 * A container is a movie collection if ALL its video subdirs are movie-like.
 *
 * This approach works regardless of how the parent folder is named (Movies,
 * Filme, Peliculas, etc.) or what language the user organises in.
 * ---------------------------------------------------------------------- */

/* Returns 1 if filename contains an SxxExx or NxNN episode pattern. */
static int has_episode_pattern(const char *name) {
    const char *p = name;
    while (*p) {
        /* SxxExx / sxxexx — e.g. S01E04, s2e10 */
        if ((p[0] == 'S' || p[0] == 's') &&
            isdigit((unsigned char)p[1])) {
            int i = 2;
            while (isdigit((unsigned char)p[i])) i++;
            if ((p[i] == 'E' || p[i] == 'e') && isdigit((unsigned char)p[i+1]))
                return 1;
        }
        /* NxNN — e.g. 1x01, 2x10 */
        if (isdigit((unsigned char)p[0]) && p[1] == 'x' &&
            isdigit((unsigned char)p[2]))
            return 1;
        p++;
    }
    return 0;
}

/* Count direct video files in path; set *has_ep=1 if any have episode patterns. */
static int count_direct_videos(const char *path, int *has_ep) {
    DIR *d = opendir(path);
    if (!d) return 0;
    int count = 0;
    *has_ep = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *child = path_join(path, ent->d_name);
        if (child) {
            if (!path_is_dir(child) && has_video_ext(ent->d_name)) {
                count++;
                if (has_episode_pattern(ent->d_name))
                    *has_ep = 1;
            }
            free(child);
        }
    }
    closedir(d);
    return count;
}

/* Returns 1 if every video-containing subdir of path has exactly 1 video
   file with no episode naming pattern — i.e., it looks like a movie
   collection rather than a TV show. */
static int all_subdirs_are_movies(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    int found_any = 0, all_movie = 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *child = path_join(path, ent->d_name);
        if (!child) continue;
        if (path_is_dir(child) && has_direct_video_files(child)) {
            int has_ep = 0;
            int cnt = count_direct_videos(child, &has_ep);
            found_any = 1;
            if (cnt != 1 || has_ep)
                all_movie = 0;
        }
        free(child);
    }
    closedir(d);
    return found_any && all_movie;
}

/* -------------------------------------------------------------------------
 * Growth helpers for MediaFolder (flat) and Season
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

static void season_add_file(Season *s, const char *dir, const char *name) {
    if (s->file_count == s->file_cap) {
        int newcap = s->file_cap ? s->file_cap * 2 : 8;
        s->files   = realloc(s->files, (size_t)newcap * sizeof(VideoFile));
        s->file_cap = newcap;
    }
    VideoFile *vf = &s->files[s->file_count++];
    vf->path = path_join(dir, name);
    vf->name = strdup(name);
}

static void show_add_season(MediaFolder *show, Season *s) {
    if (show->season_count == show->season_cap) {
        int newcap = show->season_cap ? show->season_cap * 2 : 4;
        show->seasons   = realloc(show->seasons, (size_t)newcap * sizeof(Season));
        show->season_cap = newcap;
    }
    show->seasons[show->season_count++] = *s;
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
 * Cover art lookup helper
 * ---------------------------------------------------------------------- */

static char *find_cover(const char *dir) {
    char *cjpg = path_join(dir, "cover.jpg");
    char *cpng = path_join(dir, "cover.png");
    if (cjpg && path_exists(cjpg)) { free(cpng); return cjpg; }
    if (cpng && path_exists(cpng)) { free(cjpg); return cpng; }
    free(cjpg); free(cpng);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Recursive scan
 *
 * Detection rules:
 *   1. Directory has direct video files
 *      → flat MediaFolder (is_show=0).
 *      is_movie=1 if exactly 1 video file and no episode naming pattern;
 *      is_movie=0 otherwise (multiple episode files = TV season).
 *
 *   2. Directory has no direct videos, but immediate sub-dirs that do
 *      Movie heuristic: if ALL video subdirs have exactly 1 video file
 *        and no episode patterns → movie collection: each subdir becomes
 *        its own flat MediaFolder with is_movie=1.
 *      Otherwise → TV show container (is_show=1) with those subdirs as
 *        seasons.  Single-season shows are promoted to flat.
 *
 *   3. Neither → skip this folder, recurse into sub-dirs
 * ---------------------------------------------------------------------- */

static void scan_dir(MediaLibrary *lib, const char *path) {
    DIR *d = opendir(path);
    if (!d) return;

    /* --- Pass 1: quick scan to decide which case we're in --- */
    int direct_video_count = 0;
    int direct_has_episode = 0;
    int video_subdir_count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *child = path_join(path, ent->d_name);
        if (!child) continue;
        if (path_is_dir(child)) {
            if (has_direct_video_files(child))
                video_subdir_count++;
        } else if (has_video_ext(ent->d_name)) {
            direct_video_count++;
            if (has_episode_pattern(ent->d_name))
                direct_has_episode = 1;
        }
        free(child);
    }

    /* --- Case 1: flat folder (direct video files present) --- */
    if (direct_video_count > 0) {
        MediaFolder folder = {0};
        folder.path   = strdup(path);
        const char *slash = strrchr(path, '/');
        folder.name   = strdup(slash ? slash + 1 : path);
        folder.is_show = 0;
        /* Movie heuristic: single file, no episode-style name */
        folder.is_movie = (direct_video_count == 1 && !direct_has_episode) ? 1 : 0;

        rewinddir(d);
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char *child = path_join(path, ent->d_name);
            if (!child) continue;
            if (path_is_dir(child)) {
                scan_dir(lib, child);   /* recurse into sub-dirs */
            } else if (has_video_ext(ent->d_name)) {
                folder_add_file(&folder, path, ent->d_name);
            }
            free(child);
        }

        qsort(folder.files, (size_t)folder.file_count,
              sizeof(VideoFile), videofile_cmp);
        folder.cover = find_cover(path);
        library_add_folder(lib, &folder);
        closedir(d);
        return;
    }

    /* --- Case 2 or 3: no direct videos --- */
    if (video_subdir_count == 0) {
        /* Case 3: recurse into sub-dirs only */
        rewinddir(d);
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char *child = path_join(path, ent->d_name);
            if (child && path_is_dir(child))
                scan_dir(lib, child);
            free(child);
        }
        closedir(d);
        return;
    }

    /* --- Case 2: sub-dirs with video files ---
     * Movie heuristic: if every video subdir has exactly 1 file with no
     * episode pattern, treat the container as a movie collection. */
    if (all_subdirs_are_movies(path)) {
        /* Movie collection: each video subdir → its own movie MediaFolder */
        rewinddir(d);
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char *child = path_join(path, ent->d_name);
            if (!child) continue;
            if (path_is_dir(child)) {
                if (has_direct_video_files(child)) {
                    MediaFolder movie = {0};
                    movie.path    = strdup(child);
                    movie.name    = strdup(ent->d_name);
                    movie.is_show  = 0;
                    movie.is_movie = 1;

                    DIR *md = opendir(child);
                    if (md) {
                        struct dirent *me;
                        while ((me = readdir(md)) != NULL) {
                            if (me->d_name[0] == '.') continue;
                            char *mp = path_join(child, me->d_name);
                            if (mp && !path_is_dir(mp) &&
                                has_video_ext(me->d_name))
                                folder_add_file(&movie, child, me->d_name);
                            free(mp);
                        }
                        closedir(md);
                    }

                    if (movie.file_count > 0) {
                        qsort(movie.files, (size_t)movie.file_count,
                              sizeof(VideoFile), videofile_cmp);
                        movie.cover = find_cover(child);
                        library_add_folder(lib, &movie);
                    } else {
                        free(movie.path); free(movie.name); free(movie.files);
                    }
                } else {
                    /* Deeper nesting — recurse */
                    scan_dir(lib, child);
                }
            }
            free(child);
        }
        closedir(d);
        return;
    }

    /* --- Case 2 (TV): show container — build seasons --- */
    MediaFolder show = {0};
    show.path    = strdup(path);
    const char *slash = strrchr(path, '/');
    show.name    = strdup(slash ? slash + 1 : path);
    show.is_show = 1;

    rewinddir(d);
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *child = path_join(path, ent->d_name);
        if (!child) continue;
        if (path_is_dir(child)) {
            if (has_direct_video_files(child)) {
                /* Build a season */
                Season season = {0};
                season.path = strdup(child);
                const char *s = strrchr(child, '/');
                season.name = strdup(s ? s + 1 : child);

                DIR *sd = opendir(child);
                if (sd) {
                    struct dirent *sent;
                    while ((sent = readdir(sd)) != NULL) {
                        if (sent->d_name[0] == '.') continue;
                        char *sc = path_join(child, sent->d_name);
                        if (sc && !path_is_dir(sc) &&
                            has_video_ext(sent->d_name)) {
                            season_add_file(&season, child, sent->d_name);
                        }
                        free(sc);
                    }
                    closedir(sd);
                }

                if (season.file_count > 0) {
                    qsort(season.files, (size_t)season.file_count,
                          sizeof(VideoFile), videofile_cmp);
                    season.cover = find_cover(child);
                    show_add_season(&show, &season);
                } else {
                    /* Empty season dir — free and skip */
                    free(season.path);
                    free(season.name);
                    free(season.files);
                }
            } else {
                /* Child dir has no direct videos — recurse deeper */
                scan_dir(lib, child);
            }
        }
        free(child);
    }
    closedir(d);

    if (show.season_count == 0) {
        free(show.path); free(show.name);
        return;
    }

    qsort(show.seasons, (size_t)show.season_count, sizeof(Season), season_cmp);
    show.cover = find_cover(path);

    /* Single-season promotion: skip the seasons level, treat as flat folder */
    if (show.season_count == 1) {
        Season *only = &show.seasons[0];
        show.is_show   = 0;
        show.files     = only->files;     only->files = NULL;
        show.file_count = only->file_count;
        show.file_cap   = only->file_cap;
        /* Use season cover if show has none */
        if (!show.cover && only->cover) {
            show.cover = only->cover; only->cover = NULL;
        }
        free(only->path); free(only->name); free(only->cover);
        free(show.seasons);
        show.seasons     = NULL;
        show.season_count = 0;
        show.season_cap   = 0;
    }

    library_add_folder(lib, &show);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void library_scan(MediaLibrary *lib) {
    memset(lib, 0, sizeof(*lib));
    for (int i = 0; i < SCAN_ROOT_COUNT; i++) {
        scan_dir(lib, SCAN_ROOTS[i]);
    }
    if (lib->folder_count > 1) {
        qsort(lib->folders, (size_t)lib->folder_count,
              sizeof(MediaFolder), mediafolder_cmp);
    }
}

void library_free(MediaLibrary *lib) {
    for (int i = 0; i < lib->folder_count; i++) {
        MediaFolder *f = &lib->folders[i];
        if (f->is_show) {
            for (int si = 0; si < f->season_count; si++) {
                Season *s = &f->seasons[si];
                for (int j = 0; j < s->file_count; j++) {
                    free(s->files[j].path);
                    free(s->files[j].name);
                }
                free(s->files);
                free(s->path);
                free(s->name);
                free(s->cover);
            }
            free(f->seasons);
        } else {
            for (int j = 0; j < f->file_count; j++) {
                free(f->files[j].path);
                free(f->files[j].name);
            }
            free(f->files);
        }
        free(f->path);
        free(f->name);
        free(f->cover);
    }
    free(lib->folders);
    memset(lib, 0, sizeof(*lib));
}
