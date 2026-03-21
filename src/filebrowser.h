#pragma once
#include <stddef.h>

/* Supported video file extensions */
extern const char *VIDEO_EXTENSIONS[];
extern const int   VIDEO_EXT_COUNT;

typedef struct {
    char *path;   /* full absolute path */
    char *name;   /* basename only, for display */
} VideoFile;

typedef struct {
    char      *path;         /* full absolute path to the folder */
    char      *name;         /* folder basename, for display */
    char      *cover;        /* cover.jpg/png path, or NULL if none */
    VideoFile *files;
    int        file_count;
    int        file_cap;
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
