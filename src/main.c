#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include "platform.h"
#include "filebrowser.h"
#include "browser.h"
#include "history.h"
#include "theme.h"
#include "decoder.h"
#include "audio.h"
#include "player.h"
#include "resume.h"
#include "overlay.h"
#include "statusbar.h"
#ifdef GVU_A30
#include "a30_screen.h"
#include <spawn.h>
#include <sys/wait.h>
#endif

#define FPS_CAP 60

/* -------------------------------------------------------------------------
 * SIGTERM / SIGINT handler — request clean shutdown
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_quit = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_quit = 1;
}

typedef enum {
    MODE_BROWSER,
    MODE_HISTORY,
    MODE_RESUME_PROMPT,
    MODE_UPNEXT,
    MODE_PLAYBACK,
} AppMode;

/* Navigate the browser to the folder/file that contains path. */
static void navigate_to_file(BrowserState *state, const MediaLibrary *lib,
                               const char *file_path) {
    char dir[1024];
    strncpy(dir, file_path, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';

    for (int fi = 0; fi < lib->folder_count; fi++) {
        const MediaFolder *mf = &lib->folders[fi];
        if (!mf->is_show) {
            if (strcmp(mf->path, dir) == 0) {
                state->folder_idx = fi;
                state->season_idx = 0;
                state->view       = VIEW_FILES;
                state->scroll_row = 0;
                state->selected   = 0;
                for (int i = 0; i < mf->file_count; i++) {
                    if (strcmp(mf->files[i].path, file_path) == 0) {
                        state->selected = i;
                        break;
                    }
                }
                return;
            }
        } else {
            for (int si = 0; si < mf->season_count; si++) {
                if (strcmp(mf->seasons[si].path, dir) == 0) {
                    state->folder_idx = fi;
                    state->season_idx = si;
                    state->view       = VIEW_FILES;
                    state->scroll_row = 0;
                    state->selected   = 0;
                    for (int i = 0; i < mf->seasons[si].file_count; i++) {
                        if (strcmp(mf->seasons[si].files[i].path, file_path) == 0) {
                            state->selected = i;
                            break;
                        }
                    }
                    return;
                }
            }
        }
    }
    state->view = VIEW_FOLDERS;
}

/* -------------------------------------------------------------------------
 * Helper: open + start playing a file, return 0 on success.
 * On success, handles resume-seek and updates play_folder/file_idx.
 * On failure, fills errbuf.
 * ---------------------------------------------------------------------- */

static int do_play(Player *player, const char *path, SDL_Renderer *renderer,
                   BrowserState *state, const MediaLibrary *lib,
                   int *play_folder_idx, int *play_file_idx, int *play_season_idx,
                   Uint32 *last_resume_save,
                   char *errbuf, int errsz) {
    if (player_open(player, path, renderer, errbuf, errsz) != 0)
        return -1;
    player_play(player);
    navigate_to_file(state, lib, path);
    *play_folder_idx  = state->folder_idx;
    *play_file_idx    = state->selected;
    *play_season_idx  = state->season_idx;
    *last_resume_save = SDL_GetTicks();
    return 0;
}

int main(int argc, char *argv[]) {
#ifdef GVU_A30
    int win_w = 640, win_h = 480;  /* A30 panel is always 640×480 */
    (void)argc; (void)argv;
#else
    /* Optional: ./gvu [width height]  — for testing other device resolutions.
       Supported SpruceOS resolutions: 640×480, 750×560, 1024×768, 1280×720 */
    int win_w = 640, win_h = 480;
    if (argc == 3) { win_w = atoi(argv[1]); win_h = atoi(argv[2]); }
    else (void)argv;
    (void)argc;
#endif

    signal(SIGTERM, handle_sig);
    signal(SIGINT,  handle_sig);
#ifdef GVU_A30
    signal(SIGCHLD, SIG_IGN);  /* auto-reap posix_spawn children (amixer) */
#endif

    printf("GVU — platform: %s\n", platform_name(detect_platform()));

    config_load("gvu.conf");
    printf("Theme: %s\n", theme_get()->name);
    printf("First-run done: %d\n", config_firstrun_done());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); SDL_Quit(); return 1;
    }
    if (IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG) == 0) {
        fprintf(stderr, "IMG_Init: %s\n", IMG_GetError());
        TTF_Quit(); SDL_Quit(); return 1;
    }

    SDL_Window   *win      = NULL;
    SDL_Renderer *renderer = NULL;
#ifdef GVU_A30
    /* A30: SDL_VIDEODRIVER=dummy — render into a CPU surface, flip to fb0 */
    if (a30_screen_init() != 0) {
        fprintf(stderr, "a30_screen_init failed\n");
        IMG_Quit(); TTF_Quit(); SDL_Quit(); return 1;
    }
    SDL_Surface *a30_surf = SDL_CreateRGBSurface(0, win_w, win_h, 32,
                                0x00FF0000u, 0x0000FF00u,
                                0x000000FFu, 0xFF000000u);
    if (!a30_surf) {
        fprintf(stderr, "SDL_CreateRGBSurface: %s\n", SDL_GetError());
        a30_screen_close(); IMG_Quit(); TTF_Quit(); SDL_Quit(); return 1;
    }
    renderer = SDL_CreateSoftwareRenderer(a30_surf);
#else
    win = SDL_CreateWindow("GVU",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#endif

    /* Scale font sizes with window width. Base sizes (18/14) are slightly
       larger than the original 16/13 so 640×480 also benefits. */
    int font_sz  = (int)(18.0f * win_w / 640.0f + 0.5f);
    int font_ssz = (int)(14.0f * win_w / 640.0f + 0.5f);
    TTF_Font *font       = TTF_OpenFont("resources/fonts/DejaVuSans.ttf", font_sz);
    TTF_Font *font_small = TTF_OpenFont("resources/fonts/DejaVuSans.ttf", font_ssz);

    if (!renderer || !font || !font_small) {
        fprintf(stderr, "Init error: %s / %s\n", SDL_GetError(), TTF_GetError());
        goto cleanup;
    }
#ifndef GVU_A30
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        goto cleanup;
    }
#endif

    SDL_DisableScreenSaver();

#ifdef GVU_A30
    /* Write icon.png so the SpruceOS launcher shows the current theme's cover art.
       Done at every launch so a fresh install, or a theme change made on a
       previous run, is always reflected in the app list. */
    theme_save_icon("resources/default_cover.svg", "icon.png");
#endif

    SDL_Texture *default_cover = theme_render_cover(renderer,
                                                     "resources/default_cover.svg");
    if (!default_cover)
        default_cover = IMG_LoadTexture(renderer, "resources/default_cover.png");

    printf("Scanning media library...\n");
    MediaLibrary lib;
    library_scan(&lib);
    printf("Found %d folder(s).\n", lib.folder_count);

    BrowserState state;
    CoverCache   cache;
    browser_init(&state, &cache, &lib);
    /* Restore saved layout preferences */
    state.layout        = (BrowserLayout)config_get_layout();
    state.season_layout = (BrowserLayout)config_get_season_layout();

    HistoryState history;
    memset(&history, 0, sizeof(history));

    AppMode  mode   = MODE_BROWSER;
    Player   player;
    memset(&player, 0, sizeof(player));
    Uint32   last_resume_save = 0;
    int      play_folder_idx  = 0;
    int      play_file_idx    = 0;
    int      play_season_idx  = -1;
#ifdef GVU_A30
    /* Sleep/wake detection: track SDL_GetTicks gap between frames.
       A gap > 2s while playing means the device woke from sleep. */
    Uint32 wake_prev_frame = 0;
#endif

    /* Resume-prompt state */
    char   resume_path[1024] = {0};
    double resume_pos        = 0.0;
    int    resume_folder_idx = 0;
    int    resume_file_idx   = 0;

    /* Up-Next state */
    char   upnext_path[1024] = {0};
    int    upnext_folder_idx = 0;
    int    upnext_file_idx   = 0;
    Uint32 upnext_start      = 0;
#define UPNEXT_DELAY_MS 5000

    /* Error overlay state */
    int  error_active       = 0;
    char error_path[1024]   = {0};
    char error_msg[256]     = {0};

    int          overlay_active = 0;
    SDL_Texture *help_cache     = NULL;   /* pre-rendered help overlay texture */

#ifdef GVU_A30
    /* Cover art scraping state */
    int    scrape_confirm     = 0;   /* confirmation overlay active */
    int    scrape_active      = 0;   /* scrape script running       */
    pid_t  scrape_pid         = 0;
    int    scrape_folder_idx  = -1;
    /* Season cover polling — watch for new cover.jpg files after scrape "ok" */
    int    scrape_watch_folder = -1;  /* folder being watched, -1 = inactive */
    Uint32 scrape_watch_until  = 0;   /* stop watching after this SDL tick    */
    Uint32 scrape_watch_last   = 0;   /* last time we checked (for 1s rate)   */
#define SCRAPE_DONE_FILE "/tmp/gvu_scrape_done"
#define SCRAPE_LOG_FILE  "/tmp/gvu_scrape.log"
#endif
    Uint32       menu_down_at   = 0;
    TutorialState tutorial      = { .active = !config_firstrun_done(), .slide = 0 };

    int      running  = 1;
    Uint32   frame_ms = 1000 / FPS_CAP;

    while (running) {
        /* Honor SIGTERM / SIGINT */
        if (g_quit) {
            if (player.state != PLAYER_STOPPED)
                resume_save(player.path,
                            audio_get_clock(&player.audio),
                            player.probe.duration_sec);
            running = 0;
            break;
        }

        Uint32 frame_start = SDL_GetTicks();

#ifdef GVU_A30
        /* Primary sleep/wake detection: large gap in frame timestamps.
           The main loop runs at 60 fps (~16ms/frame); a gap > 2000ms means
           the device was sleeping.  The ALSA DAC is reset to volume 0 on
           wake — reinit the audio device and restore it.
           wake_prev_frame is only updated while actively playing so that
           slow player_open calls don't look like a sleep/wake gap. */
        if (mode == MODE_PLAYBACK && player.state != PLAYER_STOPPED) {
            if (wake_prev_frame > 0 &&
                    frame_start - wake_prev_frame > 2000) {
                fprintf(stderr, "audio_wake: gap %ums — sleep/wake\n",
                        frame_start - wake_prev_frame);
                audio_wake(&player.audio);
                a30_screen_wake();
                /* Re-apply pause state — audio_wake unpauses the device */
                if (player.state == PLAYER_PAUSED)
                    SDL_PauseAudioDevice(player.audio.dev, 1);
                player_show_osd(&player);
                /* Reset the wake timer to NOW (after audio_wake returns) so
                   the time spent inside audio_wake doesn't look like another
                   sleep gap on the next frame, causing a cascade. */
                wake_prev_frame = SDL_GetTicks();
            } else {
                wake_prev_frame = frame_start;
            }
        } else {
            wake_prev_frame = 0;
        }

        a30_poll_events();
#endif

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = 0; break; }

            /* Tutorial intercepts all input */
            if (tutorial.active) {
                if (ev.type == SDL_KEYDOWN) {
                    if (!tutorial_next(&tutorial)) {
                        config_set_firstrun_done();
                        config_save("gvu.conf");
                    }
                }
                continue;
            }

            /* Help overlay: any key dismisses */
            if (overlay_active) {
                if (ev.type == SDL_KEYDOWN) {
                    overlay_active = 0;
                    if (help_cache) { SDL_DestroyTexture(help_cache); help_cache = NULL; }
                }
                continue;
            }

            /* Error overlay: any key dismisses */
            if (error_active) {
                if (ev.type == SDL_KEYDOWN)
                    error_active = 0;
                continue;
            }

            /* Intercept MENU (Escape) for hold detection */
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE
                    && !ev.key.repeat) {
                menu_down_at = SDL_GetTicks();
                continue;
            }
            if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_ESCAPE) {
                Uint32 held = menu_down_at ? SDL_GetTicks() - menu_down_at : 0;
                menu_down_at = 0;
                if (held >= 1000) {
                    overlay_active = 1;
                } else {
                    if (mode == MODE_BROWSER) {
                        state.action = BROWSER_ACTION_QUIT;
                    } else if (mode == MODE_HISTORY) {
                        history.action = HISTORY_ACTION_BACK;
                    } else if (mode == MODE_RESUME_PROMPT || mode == MODE_UPNEXT) {
                        mode = MODE_BROWSER;
                    } else { /* MODE_PLAYBACK */
                        resume_save(player.path,
                                    audio_get_clock(&player.audio),
                                    player.probe.duration_sec);
                        player_close(&player);
                        state.prog_folder_idx = -1;
                        state.prog_season_idx = -1;
                        mode = MODE_BROWSER;
                    }
                }
                continue;
            }

            if (mode == MODE_BROWSER) {
#ifdef GVU_A30
                /* Scrape confirmation: intercept A (confirm) and B (cancel) */
                if (scrape_confirm && ev.type == SDL_KEYDOWN) {
                    SDL_Keycode ck = ev.key.keysym.sym;
                    if (ck == SDLK_SPACE) {
                        /* A button — launch scrape script */
                        scrape_confirm = 0;
                        unlink(SCRAPE_DONE_FILE);
                        /* Build argv: sh resources/scrape_covers.sh <path> [key] */
                        const char *folder = lib.folders[scrape_folder_idx].path;
                        const char *key    = config_tmdb_key();
                        char *argv_buf[5];
                        argv_buf[0] = "sh";
                        argv_buf[1] = "resources/scrape_covers.sh";
                        argv_buf[2] = (char *)folder;
                        argv_buf[3] = (key && key[0]) ? (char *)key : NULL;
                        argv_buf[4] = NULL;
                        /* Redirect stdout/stderr to log file */
                        int logfd = open(SCRAPE_LOG_FILE,
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        posix_spawn_file_actions_t fa;
                        posix_spawn_file_actions_init(&fa);
                        if (logfd >= 0) {
                            posix_spawn_file_actions_adddup2(&fa, logfd, STDOUT_FILENO);
                            posix_spawn_file_actions_adddup2(&fa, logfd, STDERR_FILENO);
                            posix_spawn_file_actions_addclose(&fa, logfd);
                        }
                        if (posix_spawn(&scrape_pid, "/bin/sh", &fa, NULL,
                                        argv_buf, NULL) == 0) {
                            scrape_active = 1;
                            fprintf(stderr, "scrape: pid %d folder '%s'\n",
                                    (int)scrape_pid, folder);
                        } else {
                            perror("posix_spawn scrape");
                        }
                        posix_spawn_file_actions_destroy(&fa);
                        if (logfd >= 0) close(logfd);
                    } else if (ck == SDLK_LCTRL || ck == SDLK_BACKSPACE) {
                        scrape_confirm = 0;  /* B — cancel */
                    }
                    continue;
                }
                /* Scrape in progress: B cancels */
                if (scrape_active && ev.type == SDL_KEYDOWN) {
                    SDL_Keycode ck = ev.key.keysym.sym;
                    if (ck == SDLK_LCTRL || ck == SDLK_BACKSPACE) {
                        kill(scrape_pid, SIGTERM);
                        scrape_active = 0;
                    }
                    continue;
                }
#endif
                /* X button / Shift → open history page */
                if (ev.type == SDL_KEYDOWN &&
                    ev.key.keysym.sym == SDLK_LSHIFT) {
                    history_load(&history);
                    mode = MODE_HISTORY;
                    break;
                }

                browser_handle_event(&state, &lib, &ev);

                if (state.action == BROWSER_ACTION_QUIT) {
                    running = 0;
                } else if (state.action == BROWSER_ACTION_THEME_CYCLE) {
                    theme_cycle();
                    config_save("gvu.conf");
                    printf("Theme: %s\n", theme_get()->name);
                    if (help_cache) { SDL_DestroyTexture(help_cache); help_cache = NULL; }
                    if (default_cover) SDL_DestroyTexture(default_cover);
                    default_cover = theme_render_cover(renderer,
                                                       "resources/default_cover.svg");
                    if (!default_cover)
                        default_cover = IMG_LoadTexture(renderer,
                                                        "resources/default_cover.png");
#ifdef GVU_A30
                    theme_save_icon("resources/default_cover.svg", "icon.png");
#endif
                    state.action = BROWSER_ACTION_NONE;
                } else if (state.action == BROWSER_ACTION_PLAY) {
                    double saved = resume_load(state.action_path);
                    if (saved > 5.0) {
                        /* Show resume prompt */
                        strncpy(resume_path, state.action_path,
                                sizeof(resume_path) - 1);
                        resume_pos        = saved;
                        resume_folder_idx = state.folder_idx;
                        resume_file_idx   = state.selected;
                        mode = MODE_RESUME_PROMPT;
                    } else {
                        char errbuf[256] = {0};
                        if (do_play(&player, state.action_path, renderer,
                                    &state, &lib,
                                    &play_folder_idx, &play_file_idx, &play_season_idx,
                                    &last_resume_save,
                                    errbuf, sizeof(errbuf)) != 0) {
                            strncpy(error_path, state.action_path,
                                    sizeof(error_path) - 1);
                            strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                            error_active = 1;
                        } else {
                            mode = MODE_PLAYBACK;
                        }
                    }
                    state.action = BROWSER_ACTION_NONE;
#ifdef GVU_A30
                } else if (state.action == BROWSER_ACTION_LAYOUT_CHANGED) {
                    config_set_layout((int)state.layout);
                    config_set_season_layout((int)state.season_layout);
                    config_save("gvu.conf");
                    state.action = BROWSER_ACTION_NONE;
                } else if (state.action == BROWSER_ACTION_SCRAPE_COVERS) {
                    /* Y button — show confirmation overlay */
                    if (!scrape_active && lib.folder_count > 0) {
                        scrape_folder_idx = state.selected;
                        scrape_confirm    = 1;
                    }
                    state.action = BROWSER_ACTION_NONE;
#endif
                }

            } else if (mode == MODE_HISTORY) {
                history_handle_event(&history, &ev);

                if (history.action == HISTORY_ACTION_BACK) {
                    history_free(&history);
                    mode = MODE_BROWSER;
                    history.action = HISTORY_ACTION_NONE;
                } else if (history.action == HISTORY_ACTION_CLEAR) {
                    resume_clear_all();
                    history_free(&history);
                    history_load(&history);   /* reload — both lists now empty */
                    state.prog_folder_idx = -1;
                    state.prog_season_idx = -1;
                    history.action = HISTORY_ACTION_NONE;
                } else if (history.action == HISTORY_ACTION_PLAY) {
                    double saved = resume_load(history.action_path);
                    if (saved > 5.0) {
                        strncpy(resume_path, history.action_path,
                                sizeof(resume_path) - 1);
                        resume_pos = saved;
                        navigate_to_file(&state, &lib, history.action_path);
                        resume_folder_idx = state.folder_idx;
                        resume_file_idx   = state.selected;
                        history_free(&history);
                        mode = MODE_RESUME_PROMPT;
                    } else {
                        char errbuf[256] = {0};
                        if (do_play(&player, history.action_path, renderer,
                                    &state, &lib,
                                    &play_folder_idx, &play_file_idx, &play_season_idx,
                                    &last_resume_save,
                                    errbuf, sizeof(errbuf)) != 0) {
                            strncpy(error_path, history.action_path,
                                    sizeof(error_path) - 1);
                            strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                            error_active = 1;
                        } else {
                            history_free(&history);
                            mode = MODE_PLAYBACK;
                        }
                    }
                    history.action = HISTORY_ACTION_NONE;
                }

            } else if (mode == MODE_RESUME_PROMPT) {
                if (ev.type == SDL_KEYDOWN) {
                    char errbuf[256] = {0};
                    int  act = ev.key.keysym.sym;
                    if (act == SDLK_RETURN || act == SDLK_SPACE) {
                        /* A — resume from saved position */
                        if (do_play(&player, resume_path, renderer,
                                    &state, &lib,
                                    &play_folder_idx, &play_file_idx, &play_season_idx,
                                    &last_resume_save,
                                    errbuf, sizeof(errbuf)) != 0) {
                            strncpy(error_path, resume_path,
                                    sizeof(error_path) - 1);
                            strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                            error_active = 1;
                            mode = MODE_BROWSER;
                        } else {
                            player_seek_to(&player, resume_pos);
                            player_show_osd(&player);
                            mode = MODE_PLAYBACK;
                        }
                    } else if (act == SDLK_LSHIFT || act == SDLK_x) {
                        /* X — start from beginning */
                        if (do_play(&player, resume_path, renderer,
                                    &state, &lib,
                                    &play_folder_idx, &play_file_idx, &play_season_idx,
                                    &last_resume_save,
                                    errbuf, sizeof(errbuf)) != 0) {
                            strncpy(error_path, resume_path,
                                    sizeof(error_path) - 1);
                            strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                            error_active = 1;
                        }
                        mode = (error_active) ? MODE_BROWSER : MODE_PLAYBACK;
                    } else if (act == SDLK_LCTRL || act == SDLK_BACKSPACE) {
                        /* B — cancel */
                        mode = MODE_BROWSER;
                    }
                    (void)resume_folder_idx; (void)resume_file_idx;
                }

            } else if (mode == MODE_UPNEXT) {
                if (ev.type == SDL_KEYDOWN) {
                    int act = ev.key.keysym.sym;
                    if (act == SDLK_RETURN || act == SDLK_SPACE) {
                        /* A — play now */
                        char errbuf[256] = {0};
                        if (do_play(&player, upnext_path, renderer,
                                    &state, &lib,
                                    &play_folder_idx, &play_file_idx, &play_season_idx,
                                    &last_resume_save,
                                    errbuf, sizeof(errbuf)) != 0) {
                            strncpy(error_path, upnext_path,
                                    sizeof(error_path) - 1);
                            strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                            error_active = 1;
                            mode = MODE_BROWSER;
                        } else {
                            mode = MODE_PLAYBACK;
                        }
                    } else if (act == SDLK_LCTRL || act == SDLK_BACKSPACE) {
                        /* B — cancel, go back to browser */
                        mode = MODE_BROWSER;
                    }
                    (void)upnext_folder_idx; (void)upnext_file_idx;
                }

            } else { /* MODE_PLAYBACK */
                if (ev.type == SDL_KEYDOWN) {
                    player_show_osd(&player);
                    switch (ev.key.keysym.sym) {
                        case SDLK_SPACE:
                        case SDLK_RETURN:
                            if (player.state == PLAYER_PLAYING) player_pause(&player);
                            else if (player.state == PLAYER_PAUSED) player_resume(&player);
                            break;
                        case SDLK_LEFT:  player_seek(&player, -10.0); break;
                        case SDLK_RIGHT: player_seek(&player, +10.0); break;
                        case SDLK_PAGEUP:   player_seek(&player, -60.0); break;
                        case SDLK_PAGEDOWN: player_seek(&player, +60.0); break;
                        case SDLK_MINUS:        player_volume_dn(&player); break;
                        case SDLK_EQUALS:       player_volume_up(&player); break;
                        case SDLK_LEFTBRACKET:  player_brightness_dn(&player); break;
                        case SDLK_RIGHTBRACKET: player_brightness_up(&player); break;
                        case SDLK_UP:           player_brightness_up(&player); break;
                        case SDLK_DOWN:         player_brightness_dn(&player); break;
                        case SDLK_LALT:  player_zoom_cycle(&player);  break; /* Y */
                        case SDLK_LSHIFT: player_cycle_audio(&player); break; /* X */
                        case SDLK_RCTRL:
                        case SDLK_m:
                            player_toggle_mute(&player); break;
                        /* L2 / R2 — skip to prev / next file in folder/season */
                        case SDLK_COMMA: {
                            const MediaFolder *fol = &lib.folders[play_folder_idx];
                            const VideoFile *files;
                            int file_count;
                            if (fol->is_show && play_season_idx >= 0 &&
                                    play_season_idx < fol->season_count) {
                                files      = fol->seasons[play_season_idx].files;
                                file_count = fol->seasons[play_season_idx].file_count;
                            } else {
                                files      = fol->files;
                                file_count = fol->file_count;
                            }
                            (void)file_count;
                            if (play_file_idx > 0) {
                                int nidx = play_file_idx - 1;
                                resume_save(player.path,
                                            audio_get_clock(&player.audio),
                                            player.probe.duration_sec);
#ifdef GVU_A30
                                wake_prev_frame = 0;   /* don't treat file-open time as sleep/wake */
#endif
                                player_close(&player);
                                char errbuf[256] = {0};
                                if (do_play(&player, files[nidx].path, renderer,
                                            &state, &lib,
                                            &play_folder_idx, &play_file_idx, &play_season_idx,
                                            &last_resume_save,
                                            errbuf, sizeof(errbuf)) == 0) {
                                    double sv = resume_load(player.path);
                                    if (sv > 5.0) {
                                        player_seek_to(&player, sv);
                                        player_show_osd(&player);
                                    }
                                } else {
                                    strncpy(error_path, files[nidx].path,
                                            sizeof(error_path) - 1);
                                    strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                                    error_active = 1;
                                    mode = MODE_BROWSER;
                                    state.prog_folder_idx = -1;
                                    state.prog_season_idx = -1;
                                }
                            }
                            break;
                        }
                        case SDLK_PERIOD: {
                            const MediaFolder *fol = &lib.folders[play_folder_idx];
                            const VideoFile *files;
                            int file_count;
                            if (fol->is_show && play_season_idx >= 0 &&
                                    play_season_idx < fol->season_count) {
                                files      = fol->seasons[play_season_idx].files;
                                file_count = fol->seasons[play_season_idx].file_count;
                            } else {
                                files      = fol->files;
                                file_count = fol->file_count;
                            }
                            if (play_file_idx + 1 < file_count) {
                                int nidx = play_file_idx + 1;
                                resume_save(player.path,
                                            audio_get_clock(&player.audio),
                                            player.probe.duration_sec);
#ifdef GVU_A30
                                wake_prev_frame = 0;   /* don't treat file-open time as sleep/wake */
#endif
                                player_close(&player);
                                char errbuf[256] = {0};
                                if (do_play(&player, files[nidx].path, renderer,
                                            &state, &lib,
                                            &play_folder_idx, &play_file_idx, &play_season_idx,
                                            &last_resume_save,
                                            errbuf, sizeof(errbuf)) == 0) {
                                    double sv = resume_load(player.path);
                                    if (sv > 5.0) {
                                        player_seek_to(&player, sv);
                                        player_show_osd(&player);
                                    }
                                } else {
                                    strncpy(error_path, files[nidx].path,
                                            sizeof(error_path) - 1);
                                    strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                                    error_active = 1;
                                    mode = MODE_BROWSER;
                                    state.prog_folder_idx = -1;
                                    state.prog_season_idx = -1;
                                }
                            }
                            break;
                        }
                        case SDLK_LCTRL:
                        case SDLK_BACKSPACE:
                            resume_save(player.path,
                                        audio_get_clock(&player.audio),
                                        player.probe.duration_sec);
                            player_close(&player);
                            state.prog_folder_idx = -1;
                            state.prog_season_idx = -1;
                            mode = MODE_BROWSER;
                            break;
                        default: break;
                    }
                }
            }
        }

        /* Update video frame (A/V sync) */
        if (mode == MODE_PLAYBACK) {
            player_update(&player);
            if (player.eos) {
                resume_record_completed(player.path);
                resume_clear(player.path);
                player_close(&player);

                /* Check if there is a next file in the same folder/season */
                const MediaFolder *folder = &lib.folders[play_folder_idx];
                const VideoFile *eos_files;
                int eos_count;
                if (folder->is_show && play_season_idx >= 0 &&
                        play_season_idx < folder->season_count) {
                    eos_files = folder->seasons[play_season_idx].files;
                    eos_count = folder->seasons[play_season_idx].file_count;
                } else {
                    eos_files = folder->files;
                    eos_count = folder->file_count;
                }
                int next = play_file_idx + 1;
                if (next < eos_count) {
                    strncpy(upnext_path, eos_files[next].path,
                            sizeof(upnext_path) - 1);
                    upnext_folder_idx = play_folder_idx;
                    upnext_file_idx   = next;
                    upnext_start      = SDL_GetTicks();
                    mode = MODE_UPNEXT;
                } else {
                    state.prog_folder_idx = -1;
                    state.prog_season_idx = -1;
                    mode = MODE_BROWSER;
                }
            } else {
                Uint32 now = SDL_GetTicks();
                if (player.state == PLAYER_PLAYING &&
                    now - last_resume_save >= 15000) {
                    resume_save(player.path,
                                audio_get_clock(&player.audio),
                                player.probe.duration_sec);
                    last_resume_save = now;
                }
            }
        }

        /* Up-Next auto-advance countdown */
        if (mode == MODE_UPNEXT) {
            Uint32 elapsed = SDL_GetTicks() - upnext_start;
            if (elapsed >= UPNEXT_DELAY_MS) {
                char errbuf[256] = {0};
                if (do_play(&player, upnext_path, renderer,
                            &state, &lib,
                            &play_folder_idx, &play_file_idx, &play_season_idx,
                            &last_resume_save,
                            errbuf, sizeof(errbuf)) != 0) {
                    strncpy(error_path, upnext_path, sizeof(error_path) - 1);
                    strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                    error_active = 1;
                    state.prog_folder_idx = -1;
                    state.prog_season_idx = -1;
                    mode = MODE_BROWSER;
                } else {
                    mode = MODE_PLAYBACK;
                }
            }
        }

#ifdef GVU_A30
        /* Poll for scrape completion via sentinel file */
        if (scrape_active && access(SCRAPE_DONE_FILE, F_OK) == 0) {
            scrape_active = 0;
            unlink(SCRAPE_DONE_FILE);
            fprintf(stderr, "scrape: done for folder %d\n", scrape_folder_idx);
            /* Update lib cover pointer if cover.jpg now exists */
            if (scrape_folder_idx >= 0 && scrape_folder_idx < lib.folder_count) {
                MediaFolder *sf = &lib.folders[scrape_folder_idx];
                char cp[1200];
                snprintf(cp, sizeof(cp), "%s/cover.jpg", sf->path);
                if (access(cp, F_OK) == 0) {
                    free(sf->cover);
                    sf->cover = strdup(cp);
                }
                /* Invalidate cached texture so it reloads on next draw */
                if (scrape_folder_idx < cache.count &&
                    cache.textures[scrape_folder_idx]) {
                    SDL_DestroyTexture(cache.textures[scrape_folder_idx]);
                    cache.textures[scrape_folder_idx] = NULL;
                }
                if (cache.backdrop_idx == scrape_folder_idx) {
                    if (cache.backdrop) SDL_DestroyTexture(cache.backdrop);
                    cache.backdrop     = NULL;
                    cache.backdrop_idx = -1;
                }
                /* Invalidate season texture cache and start polling for
                   season covers (script writes "ok" before scraping seasons) */
                if (cache.season_tex_folder_idx == scrape_folder_idx) {
                    for (int si = 0; si < cache.season_tex_count; si++)
                        if (cache.season_textures[si])
                            SDL_DestroyTexture(cache.season_textures[si]);
                    free(cache.season_textures);
                    cache.season_textures    = NULL;
                    cache.season_tex_count   = 0;
                    cache.season_tex_folder_idx = -1;
                }
                scrape_watch_folder = scrape_folder_idx;
                scrape_watch_until  = SDL_GetTicks() + 60000; /* watch 60 s */
                scrape_watch_last   = 0;
            }
            scrape_folder_idx = -1;
        }

        /* Poll for new season cover.jpg files after scrape (once per second) */
        if (scrape_watch_folder >= 0) {
            Uint32 now_w = SDL_GetTicks();
            if (now_w >= scrape_watch_until) {
                scrape_watch_folder = -1;  /* window expired */
            } else if (now_w - scrape_watch_last >= 1000) {
                scrape_watch_last = now_w;
                MediaFolder *wf = &lib.folders[scrape_watch_folder];
                int changed = 0;
                for (int si = 0; si < wf->season_count; si++) {
                    Season *s = &wf->seasons[si];
                    if (!s->cover) {
                        char scp[1200];
                        snprintf(scp, sizeof(scp), "%s/cover.jpg", s->path);
                        if (access(scp, F_OK) == 0) {
                            s->cover = strdup(scp);
                            changed  = 1;
                        }
                    }
                }
                if (changed && cache.season_tex_folder_idx == scrape_watch_folder) {
                    for (int si = 0; si < cache.season_tex_count; si++)
                        if (cache.season_textures[si])
                            SDL_DestroyTexture(cache.season_textures[si]);
                    free(cache.season_textures);
                    cache.season_textures       = NULL;
                    cache.season_tex_count      = 0;
                    cache.season_tex_folder_idx = -1;
                }
            }
        }
#endif

        /* Render */
        int sbar_h = statusbar_height(win_w);
        if (mode == MODE_BROWSER || mode == MODE_RESUME_PROMPT ||
            mode == MODE_HISTORY  || mode == MODE_UPNEXT) {

            /* Browser/history modes: push content below the status bar */
            SDL_Rect content_vp = {0, sbar_h, win_w, win_h - sbar_h};
            SDL_RenderSetViewport(renderer, &content_vp);

            if (mode == MODE_HISTORY) {
                history_draw(renderer, font, font_small, &history,
                             theme_get(), win_w, win_h - sbar_h);
            } else {
                browser_draw(renderer, font, font_small,
                             &state, &cache, &lib, default_cover,
                             theme_get(), win_w, win_h - sbar_h);
            }

            SDL_RenderSetViewport(renderer, NULL);
            statusbar_draw(renderer, font, theme_get(), win_w, win_h);

        } else {
            /* Playback mode: full-screen, status bar only when OSD is visible */
            player_draw(renderer, font, font_small, &player, theme_get(), win_w, win_h);
            if (player.osd_visible)
                statusbar_draw(renderer, font, theme_get(), win_w, win_h);
        }

        /* Overlay layers */
        if (overlay_active) {
            /* Build the help texture once; blit every frame instead of
               re-rendering 16+ TTF glyphs per frame (too slow on ARMv7). */
            if (!help_cache) {
                help_cache = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_TARGET, win_w, win_h);
                if (help_cache) {
                    SDL_SetRenderTarget(renderer, help_cache);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
                    SDL_RenderClear(renderer);
                    help_draw(renderer, font, font_small, theme_get(), win_w, win_h);
                    SDL_SetRenderTarget(renderer, NULL);
                    SDL_SetTextureBlendMode(help_cache, SDL_BLENDMODE_BLEND);
                }
            }
            if (help_cache)
                SDL_RenderCopy(renderer, help_cache, NULL, NULL);
            else
                help_draw(renderer, font, font_small, theme_get(), win_w, win_h);
        } else if (tutorial.active) {
            tutorial_draw(renderer, font, font_small, &tutorial, theme_get(), win_w, win_h);
        } else if (mode == MODE_RESUME_PROMPT) {
            resume_prompt_draw(renderer, font, font_small, theme_get(), win_w, win_h,
                               resume_path, resume_pos);
        } else if (mode == MODE_UPNEXT) {
            int secs_left = (int)((UPNEXT_DELAY_MS - (SDL_GetTicks() - upnext_start) + 999) / 1000);
            if (secs_left < 0) secs_left = 0;
            upnext_draw(renderer, font, font_small, theme_get(), win_w, win_h,
                        upnext_path, secs_left);
        }
        if (error_active) {
            error_draw(renderer, font, font_small, theme_get(), win_w, win_h,
                       error_path, error_msg);
        }
#ifdef GVU_A30
        /* Scrape confirmation overlay */
        if (scrape_confirm && scrape_folder_idx >= 0 &&
                scrape_folder_idx < lib.folder_count) {
            const Theme *t = theme_get();
            /* Dim background */
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
            SDL_Rect full = { 0, 0, win_w, win_h };
            SDL_RenderFillRect(renderer, &full);
            /* Panel */
            int pw = win_w * 3 / 4, ph = win_h * 2 / 5;
            int px = (win_w - pw) / 2, py = (win_h - ph) / 2;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, t->background.r, t->background.g,
                                   t->background.b, 255);
            SDL_Rect panel = { px, py, pw, ph };
            SDL_RenderFillRect(renderer, &panel);
            SDL_SetRenderDrawColor(renderer, t->highlight_bg.r,
                                   t->highlight_bg.g, t->highlight_bg.b, 255);
            SDL_RenderDrawRect(renderer, &panel);
            /* Title */
            SDL_Color tc = { t->text.r, t->text.g, t->text.b, 255 };
            SDL_Surface *ts = TTF_RenderUTF8_Blended(font, "Fetch Cover Art?", tc);
            if (ts) {
                SDL_Texture *tt = SDL_CreateTextureFromSurface(renderer, ts);
                if (tt) {
                    SDL_Rect tr = { px + (pw - ts->w) / 2, py + ph / 6, ts->w, ts->h };
                    SDL_RenderCopy(renderer, tt, NULL, &tr);
                    SDL_DestroyTexture(tt);
                }
                SDL_FreeSurface(ts);
            }
            /* Folder name */
            SDL_Color sc2 = { t->secondary.r, t->secondary.g, t->secondary.b, 255 };
            const char *fn = lib.folders[scrape_folder_idx].name;
            SDL_Surface *fs = TTF_RenderUTF8_Blended(font_small, fn, sc2);
            if (fs) {
                SDL_Texture *ft = SDL_CreateTextureFromSurface(renderer, fs);
                if (ft) {
                    int fw = fs->w < pw - 16 ? fs->w : pw - 16;
                    SDL_Rect fr = { px + (pw - fw) / 2, py + ph * 2 / 5, fw, fs->h };
                    SDL_RenderCopy(renderer, ft, NULL, &fr);
                    SDL_DestroyTexture(ft);
                }
                SDL_FreeSurface(fs);
            }
            /* Attribution */
            SDL_Surface *as = TTF_RenderUTF8_Blended(font_small,
                "Sources: TMDB, TVMaze", sc2);
            if (as) {
                SDL_Texture *at = SDL_CreateTextureFromSurface(renderer, as);
                if (at) {
                    SDL_Rect ar = { px + (pw - as->w) / 2, py + ph * 3 / 5, as->w, as->h };
                    SDL_RenderCopy(renderer, at, NULL, &ar);
                    SDL_DestroyTexture(at);
                }
                SDL_FreeSurface(as);
            }
            /* Hint */
            SDL_Color hc = { t->highlight_text.r, t->highlight_text.g,
                             t->highlight_text.b, 255 };
            SDL_Surface *hs = TTF_RenderUTF8_Blended(font_small, "A: Fetch   B: Cancel", hc);
            if (hs) {
                SDL_Texture *ht = SDL_CreateTextureFromSurface(renderer, hs);
                if (ht) {
                    SDL_Rect hr = { px + (pw - hs->w) / 2,
                                    py + ph - hs->h - 8, hs->w, hs->h };
                    SDL_RenderCopy(renderer, ht, NULL, &hr);
                    SDL_DestroyTexture(ht);
                }
                SDL_FreeSurface(hs);
            }
        }
        /* Scrape progress overlay */
        if (scrape_active) {
            const Theme *t = theme_get();
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
            SDL_Rect full = { 0, 0, win_w, win_h };
            SDL_RenderFillRect(renderer, &full);
            /* Panel */
            int pw = win_w * 3 / 4, ph = win_h / 5;
            int px = (win_w - pw) / 2, py = (win_h - ph) / 2;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, t->background.r, t->background.g,
                                   t->background.b, 255);
            SDL_Rect panel = { px, py, pw, ph };
            SDL_RenderFillRect(renderer, &panel);
            SDL_SetRenderDrawColor(renderer, t->highlight_bg.r,
                                   t->highlight_bg.g, t->highlight_bg.b, 255);
            SDL_RenderDrawRect(renderer, &panel);
            /* Animated dots */
            static int s_dot_frame = 0;
            s_dot_frame = (s_dot_frame + 1) % 60;
            int ndots = 1 + s_dot_frame / 20;
            char msg[32];
            snprintf(msg, sizeof(msg), "Fetching cover art%.*s", ndots, "...");
            SDL_Color tc = { t->text.r, t->text.g, t->text.b, 255 };
            SDL_Surface *ts = TTF_RenderUTF8_Blended(font, msg, tc);
            if (ts) {
                SDL_Texture *tt = SDL_CreateTextureFromSurface(renderer, ts);
                if (tt) {
                    SDL_Rect tr = { px + (pw - ts->w) / 2,
                                    py + (ph - ts->h) / 2, ts->w, ts->h };
                    SDL_RenderCopy(renderer, tt, NULL, &tr);
                    SDL_DestroyTexture(tt);
                }
                SDL_FreeSurface(ts);
            }
            SDL_Color sc2 = { t->secondary.r, t->secondary.g, t->secondary.b, 255 };
            SDL_Surface *cs = TTF_RenderUTF8_Blended(font_small, "B: Cancel", sc2);
            if (cs) {
                SDL_Texture *ct = SDL_CreateTextureFromSurface(renderer, cs);
                if (ct) {
                    SDL_Rect cr = { px + (pw - cs->w) / 2,
                                    py + ph - cs->h - 4, cs->w, cs->h };
                    SDL_RenderCopy(renderer, ct, NULL, &cr);
                    SDL_DestroyTexture(ct);
                }
                SDL_FreeSurface(cs);
            }
        }
#endif

        SDL_RenderPresent(renderer);
#ifdef GVU_A30
        a30_flip(a30_surf);
#endif

        Uint32 elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
    }

    /* Cleanup */
    if (player.state != PLAYER_STOPPED) {
        resume_save(player.path,
                    audio_get_clock(&player.audio),
                    player.probe.duration_sec);
        player_close(&player);
    }
    history_free(&history);
    browser_state_free(&state);
    cover_cache_free(&cache);
    library_free(&lib);
    if (default_cover) SDL_DestroyTexture(default_cover);
    if (help_cache)    SDL_DestroyTexture(help_cache);

cleanup:
    if (font_small) TTF_CloseFont(font_small);
    if (font)       TTF_CloseFont(font);
    if (renderer)   SDL_DestroyRenderer(renderer);
#ifdef GVU_A30
    if (a30_surf)   SDL_FreeSurface(a30_surf);
    a30_screen_close();
#else
    if (win)        SDL_DestroyWindow(win);
#endif
    IMG_Quit(); TTF_Quit(); SDL_Quit();
    return 0;
}
