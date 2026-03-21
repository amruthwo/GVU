#pragma once
#include <stddef.h>

/* Supported video file extensions */
extern const char *VIDEO_EXTENSIONS[];
extern const int   VIDEO_EXT_COUNT;

typedef struct {
    char *path;   /* full absolute path */
    char *name;   /* basename only, for display */
} VideoFile;

/* A season sub-directory inside a show container */
typedef struct {
    char      *path;
    char      *name;        /* e.g. "Season 1" */
    char      *cover;       /* cover.jpg/png, or NULL */
    VideoFile *files;
    int        file_count;
    int        file_cap;
} Season;

typedef struct {
    char      *path;
    char      *name;
    char      *cover;

    /* is_show == 0: flat folder (files/file_count/file_cap used) */
    /* is_show == 1: show container (seasons/season_count/season_cap used) */
    int        is_show;

    VideoFile *files;
    int        file_count;
    int        file_cap;

    Season    *seasons;
    int        season_count;
    int        season_cap;
} MediaFolder;

typedef struct {
    MediaFolder *folders;
    int          folder_count;
    int          folder_cap;
} MediaLibrary;

/* Scan both root paths and populate lib.
   Caller must call library_free() when done. */
void library_scan(MediaLibrary *lib);

/* Free all memory owned by lib */
void library_free(MediaLibrary *lib);
