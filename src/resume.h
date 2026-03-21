#pragma once

/* Per-file resume positions, persisted to a flat text file.
   Format: one entry per line — "path\tposition_seconds\tduration_seconds\n"
   (duration field is optional; old 2-field lines are accepted with duration=0)

   Updated entries are moved to the end of the file so the most-recently-
   active entry is always last (history page reads in reverse for "most recent
   first" ordering).

   resume_save            — write/update position (pass duration or 0.0)
   resume_load            — return saved position, or -1.0 if none
   resume_clear           — remove entry (call on EOS)
   resume_load_all        — load all entries, most-recent first (history page)
   resume_record_completed — append path to history.dat (capped at 64)
   resume_load_completed  — load recently-completed paths, most-recent first */

typedef struct {
    char   path[1024];
    double position;   /* seconds watched */
    double duration;   /* total duration; 0.0 if unknown */
} ResumeEntry;

void   resume_save            (const char *path, double pos_sec, double duration_sec);
double resume_load            (const char *path);
void   resume_clear           (const char *path);

/* Load all in-progress entries, most-recent first.
   Returns count; caller must free(*out). */
int    resume_load_all        (ResumeEntry **out);

/* Record a completed file in history.dat (deduped, capped at 64). */
void   resume_record_completed(const char *path);

/* Load recently-completed paths, most-recent first.
   Returns count; caller must free(*out) (heap array of char[1024]). */
int    resume_load_completed  (char (**out)[1024]);
