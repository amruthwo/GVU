/*
 * fetch_subs.c — GVU subtitle fetcher (C replacement for fetch_subtitles.py)
 *
 * Self-contained binary using libcurl (static, mbedTLS backend).
 * No Python required.
 *
 * Usage:
 *   fetch_subs search   <video_path> <subdl_key> <lang>
 *   fetch_subs download <provider>   <download_key> <srt_dest>
 *
 * Output files:
 *   /tmp/gvu_sub_results.txt  — pipe-delimited: provider|key|name|lang|downloads|hi
 *   /tmp/gvu_sub_done         — "ok" or "error: <message>"
 *
 * Env vars read:
 *   GVU_CACERT_PATH — path to CA bundle (falls back to SpruceOS system CAs)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* getentropy is absent on glibc < 2.25 (e.g. Miyoo A30).
 * OpenSSL uses it for entropy; provide it here so the linker resolves the
 * reference locally and does not create a dynamic dependency on libc. */
#if defined(__linux__)
int getentropy(void *buf, size_t buflen);
int getentropy(void *buf, size_t buflen) {
    if (buflen > 256) { errno = EIO; return -1; }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < buflen) {
        ssize_t n = read(fd, (char *)buf + got, buflen - got);
        if (n <= 0) { close(fd); errno = EIO; return -1; }
        got += (size_t)n;
    }
    close(fd);
    return 0;
}
#endif
#include <regex.h>
#include <strings.h>      /* strcasecmp, strncasecmp */
#include <curl/curl.h>
#include <zlib.h>

#define RESULTS_FILE     "/tmp/gvu_sub_results.txt"
#define DONE_FILE        "/tmp/gvu_sub_done"
#define SUBDL_DL_BASE    "https://dl.subdl.com"
#define SUB_RESULT_MAX   32
#define HTTP_TIMEOUT_S   20L
#define DL_TIMEOUT_S     60L

/* Fallback CA bundle on SpruceOS devices that lack a shipped cacert.pem */
#define SPRUCE_CA_BUNDLE "/mnt/SDCARD/spruce/etc/ca-certificates.crt"

/* ── CA bundle path ────────────────────────────────────────────────────────── */

/* OpenSSL backend supports CURLOPT_CAINFO (file path) directly.
   Just find which CA bundle file exists and store the path. */

static const char *g_ca_path = NULL;

static void ca_bundle_load(void)
{
    static const char *paths[2];
    paths[0] = getenv("GVU_CACERT_PATH");
    paths[1] = SPRUCE_CA_BUNDLE;
    for (int i = 0; i < 2; i++) {
        const char *p = paths[i];
        if (!p || !p[0]) continue;
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fclose(f);
        g_ca_path = p;
        fprintf(stderr, "ca: using %s\n", p);
        return;
    }
    fprintf(stderr, "ca: WARNING — no CA bundle found, SSL peer verification will fail\n");
}

/* ── HTTP response buffer ──────────────────────────────────────────────────── */

typedef struct { char *data; size_t size; } MemBuf;

static size_t membuf_write_cb(void *ptr, size_t sz, size_t n, void *ud)
{
    MemBuf *mb  = (MemBuf *)ud;
    size_t  add = sz * n;
    char   *p   = realloc(mb->data, mb->size + add + 1);
    if (!p) return 0;
    mb->data = p;
    memcpy(mb->data + mb->size, ptr, add);
    mb->size += add;
    mb->data[mb->size] = '\0';
    return add;
}

static void membuf_free(MemBuf *mb) { free(mb->data); mb->data = NULL; mb->size = 0; }

/* ── URL encoding ─────────────────────────────────────────────────────────── */

static void url_encode(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ') {
            out[j++] = '+';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            j += (size_t)snprintf(out + j, out_sz - j, "%%%02X", (unsigned)c);
        }
    }
    out[j] = '\0';
}

/* ── libcurl helpers ──────────────────────────────────────────────────────── */

/* Apply common options to a curl handle */
static void curl_setup_common(CURL *curl, long timeout_s)
{
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* Our static OpenSSL 1.1.1w (linux-armv4 target) fails ECDSA chain
       verification on ARM32 even with a valid CA bundle — the SpruceOS wget
       (dynamic OpenSSL) verifies the same chain fine.  For subtitle downloads
       (low-sensitivity public data) skipping chain verification is acceptable.
       Hostname verification is kept at the libcurl level as a sanity check. */
#ifdef GVU_A30
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#else
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (g_ca_path)
        curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_path);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GVU/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);  /* fail fast if host unreachable */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
}

/*
 * HTTP GET: fetch url into mb.  On success returns CURLE_OK and sets *http_code.
 */
static CURLcode http_get(const char *url, struct curl_slist *extra_hdrs,
                          long *http_code, MemBuf *mb)
{
    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;
    curl_setup_common(curl, HTTP_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, mb);
    if (extra_hdrs)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, extra_hdrs);
    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_easy_cleanup(curl);
    return rc;
}

/*
 * HTTP download: write url directly to dest_path.
 * Returns 1 on success (HTTP 200, file > 100 bytes).
 */
static int http_download(const char *url, const char *dest_path)
{
    FILE *f = fopen(dest_path, "wb");
    if (!f) { perror(dest_path); return 0; }

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(f); return 0; }
    curl_setup_common(curl, DL_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);   /* default fwrite */
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(f);

    if (rc != CURLE_OK || http_code != 200) {
        fprintf(stderr, "download: curl=%d http=%ld url=%s\n", rc, http_code, url);
        return 0;
    }
    struct stat st;
    return (stat(dest_path, &st) == 0 && st.st_size > 100);
}

/* ── Minimal JSON helpers ────────────────────────────────────────────────── */
/*
 * Not a full parser — just enough for SubDL and Podnapisi flat object fields.
 * json_str_val() and json_int_val() search within a single object's text
 * (extracted by json_array_each), so false-positive key matches are unlikely.
 */

static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Copy a JSON quoted string value starting at '"' into buf[buf_sz].
   Returns pointer after closing '"', or NULL on failure. */
static const char *json_copy_string(const char *p, char *buf, size_t buf_sz)
{
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            switch (*p) {
            case '"': case '\\': case '/':
                if (i + 1 < buf_sz) buf[i++] = *p;
                break;
            case 'n':
                if (i + 1 < buf_sz) buf[i++] = '\n';
                break;
            case 'r':
                if (i + 1 < buf_sz) buf[i++] = '\r';
                break;
            case 't':
                if (i + 1 < buf_sz) buf[i++] = '\t';
                break;
            default:
                if (i + 1 < buf_sz) buf[i++] = *p;
                break;
            }
        } else {
            if (i + 1 < buf_sz) buf[i++] = *p;
        }
        p++;
    }
    if (*p == '"') p++;
    buf[i < buf_sz ? i : buf_sz - 1] = '\0';
    return p;
}

/*
 * Find "key": value within json and copy value string to out[out_sz].
 * Handles both quoted strings and unquoted tokens (numbers, booleans, null).
 * Returns 1 on success, 0 if key not found.
 */
static int json_str_val(const char *json, const char *key,
                         char *out, size_t out_sz)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    p  = json_skip_ws(p);
    if (*p != ':') return 0;
    p++;
    p = json_skip_ws(p);
    if (*p == '"')
        return json_copy_string(p, out, out_sz) != NULL;
    /* Unquoted value: copy to delimiter */
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' '  && *p != '\n' && *p != '\r') {
        if (i + 1 < out_sz) out[i++] = *p;
        p++;
    }
    out[i < out_sz ? i : out_sz - 1] = '\0';
    return (i > 0);
}

static int json_int_val(const char *json, const char *key, int *out)
{
    char tmp[32];
    if (!json_str_val(json, key, tmp, sizeof(tmp))) return 0;
    *out = atoi(tmp);
    return 1;
}

/*
 * Iterate items in the JSON array at "key", calling cb for each '{...}' object.
 * cb receives a NUL-terminated string of the object text and its length.
 */
typedef void (*json_array_cb)(const char *item, size_t item_len, void *ud);

static void json_array_each(const char *json, const char *key,
                              json_array_cb cb, void *ud)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return;
    p += strlen(pat);
    p  = json_skip_ws(p);
    if (*p != ':') return;
    p++;
    p = json_skip_ws(p);
    if (*p != '[') return;
    p++;  /* skip '[' */

    while (*p) {
        p = json_skip_ws(p);
        if (*p == ']') break;
        if (*p != '{') { p++; continue; }

        /* Find matching '}', respecting nesting and string escapes */
        const char *start  = p;
        int         depth  = 0;
        int         in_str = 0;
        const char *q      = p;
        while (*q) {
            if (in_str) {
                if (*q == '\\' && *(q+1)) { q += 2; continue; }
                if (*q == '"')  in_str = 0;
            } else {
                if (*q == '"')  in_str = 1;
                else if (*q == '{') depth++;
                else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            }
            q++;
        }

        /* Build NUL-terminated copy for cb */
        size_t  item_len = (size_t)(q - start);
        char   *item_buf = malloc(item_len + 1);
        if (item_buf) {
            memcpy(item_buf, start, item_len);
            item_buf[item_len] = '\0';
            cb(item_buf, item_len, ud);
            free(item_buf);
        }
        p = q;
        p = json_skip_ws(p);
        if (*p == ',') p++;
    }
}

/* ── Filename parser ─────────────────────────────────────────────────────── */

static const char *RELEASE_TAGS[] = {
    "720p","1080p","2160p","4K","HDR","SDR",
    "WEBRip","WEBDL","WEB-DL","BluRay","BDRip","DVDRip",
    "x264","x265","H264","H265","HEVC","AVC","AAC","AC3",
    "DDP","DTS","FLAC","AMZN","HULU","NF","DSNP","ATVP",
    "WEB","HDTV","PROPER","REPACK","EXTENDED","YTS","RARBG",
    "YIFY","EAC3","TrueHD",
    NULL
};

/* Strip a known tag (case-insensitive, word-boundary) from title in place. */
static void strip_tag(char *title, const char *tag)
{
    size_t tlen = strlen(tag);
    char  *p    = title;
    while (*p) {
        if (strncasecmp(p, tag, tlen) == 0) {
            int before_ok = (p == title || !isalnum((unsigned char)*(p - 1)));
            int after_ok  = (!isalnum((unsigned char)*(p + tlen)));
            if (before_ok && after_ok) {
                memmove(p, p + tlen, strlen(p + tlen) + 1);
                continue;
            }
        }
        p++;
    }
}

/* Convert dots/underscores to spaces, collapse multiple spaces, trim. */
static void normalize_separators(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == '.' || *p == '_') *p = ' ';
    char *r = s, *w = s;
    int   sp = 0;
    while (*r) {
        if (*r == ' ') { if (!sp) { *w++ = ' '; sp = 1; } }
        else           { *w++ = *r; sp = 0; }
        r++;
    }
    *w = '\0';
    /* Trim leading separators */
    while (*s == ' ' || *s == '-' || *s == '(' || *s == '[')
        memmove(s, s + 1, strlen(s));
    /* Trim trailing separators */
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == ' ' || s[l-1] == '-'))
        s[--l] = '\0';
}

/* Compile the S##E## pattern once. */
static regex_t  s_re_sxxexx;
static int      s_re_compiled = 0;

static void ensure_re(void)
{
    if (!s_re_compiled) {
        regcomp(&s_re_sxxexx, "[Ss]([0-9]{1,2})[Ee]([0-9]{1,3})", REG_EXTENDED);
        s_re_compiled = 1;
    }
}

/* Try to extract season/episode from normalized string. -1 if not found. */
static int try_se(const char *norm, int *season, int *episode)
{
    ensure_re();
    regmatch_t m[3];
    if (regexec(&s_re_sxxexx, norm, 3, m, 0) != 0) return 0;
    char sn[8], en[8];
    snprintf(sn, sizeof(sn), "%.*s",
             (int)(m[1].rm_eo - m[1].rm_so), norm + m[1].rm_so);
    snprintf(en, sizeof(en), "%.*s",
             (int)(m[2].rm_eo - m[2].rm_so), norm + m[2].rm_so);
    *season  = atoi(sn);
    *episode = atoi(en);
    return 1;
}

/*
 * Extract (title, season, episode) from a video file path.
 * season / episode are -1 if this is a movie (no episode tag found).
 */
static void parse_filename(const char *video_path,
                            char *title, size_t title_sz,
                            int *season, int *episode, int *year)
{
    *season = *episode = *year = -1;

    /* Basename without extension */
    const char *base = strrchr(video_path, '/');
    base = base ? base + 1 : video_path;
    char stem[512];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    normalize_separators(stem);

    /* Look for S##E## in filename */
    regmatch_t m[3];
    ensure_re();
    if (regexec(&s_re_sxxexx, stem, 3, m, 0) == 0) {
        char sn[8], en[8];
        snprintf(sn, sizeof(sn), "%.*s",
                 (int)(m[1].rm_eo - m[1].rm_so), stem + m[1].rm_so);
        snprintf(en, sizeof(en), "%.*s",
                 (int)(m[2].rm_eo - m[2].rm_so), stem + m[2].rm_so);
        *season  = atoi(sn);
        *episode = atoi(en);
        stem[m[0].rm_so] = '\0';
        normalize_separators(stem);
    }

    /* If no episode in filename, check the parent directory name */
    if (*season < 0) {
        char parent_buf[512];
        snprintf(parent_buf, sizeof(parent_buf), "%s", video_path);
        char *sl = strrchr(parent_buf, '/');
        if (sl) {
            *sl = '\0';
            sl  = strrchr(parent_buf, '/');
            if (sl) {
                char pnorm[512];
                snprintf(pnorm, sizeof(pnorm), "%s", sl + 1);
                normalize_separators(pnorm);
                try_se(pnorm, season, episode);
            }
        }
    }

    /* Strip release tags */
    for (int i = 0; RELEASE_TAGS[i]; i++)
        strip_tag(stem, RELEASE_TAGS[i]);
    normalize_separators(stem);

    /* Strip year "(YYYY)" from movie titles — not a release tag but confuses
       subtitle APIs.  Only strip for movies (season < 0); TV episode filenames
       with a year in them are unusual and stripping is probably still fine.
       Capture the year value before removing it so we can pass it to the API. */
    {
        char *yp = stem;
        while (*yp) {
            if (*yp == '(' && isdigit((unsigned char)yp[1]) &&
                isdigit((unsigned char)yp[2]) && isdigit((unsigned char)yp[3]) &&
                isdigit((unsigned char)yp[4]) && yp[5] == ')') {
                if (*year < 0) {
                    char yr_buf[5];
                    snprintf(yr_buf, sizeof(yr_buf), "%.*s", 4, yp + 1);
                    *year = atoi(yr_buf);
                }
                memmove(yp, yp + 6, strlen(yp + 6) + 1);
                continue;
            }
            yp++;
        }
        normalize_separators(stem);
    }

    /* If title empty and we have season info, climb directory tree for show name */
    if (stem[0] == '\0' && *season >= 0) {
        char parent_buf[512];
        snprintf(parent_buf, sizeof(parent_buf), "%s", video_path);
        char *sl = strrchr(parent_buf, '/');
        if (sl) {
            *sl = '\0';
            sl  = strrchr(parent_buf, '/');
            if (sl) {
                char *dir_name = sl + 1;
                /* If this looks like "Season N" or "S##", go up one more */
                if (strncasecmp(dir_name, "Season", 6) == 0 ||
                    (dir_name[0] == 'S' && isdigit((unsigned char)dir_name[1]))) {
                    *sl = '\0';
                    sl  = strrchr(parent_buf, '/');
                    dir_name = sl ? sl + 1 : parent_buf;
                }
                snprintf(stem, sizeof(stem), "%s", dir_name);
                normalize_separators(stem);
                for (int i = 0; RELEASE_TAGS[i]; i++)
                    strip_tag(stem, RELEASE_TAGS[i]);
                normalize_separators(stem);
            }
        }
    }

    snprintf(title, title_sz, "%s", stem);
}

/* ── Minimal ZIP reader (Central Directory based) ────────────────────────── */

static uint16_t u16le(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t u32le(const unsigned char *p)
{
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

typedef struct {
    char     name[512];
    uint16_t compress;    /* 0=stored, 8=deflated */
    uint32_t comp_sz;
    uint32_t uncomp_sz;
    uint32_t local_hdr_off; /* relative offset of local file header in zip */
} ZipEntry;

/*
 * Locate the End of Central Directory record.
 * Sets *cd_off to offset of Central Directory, *num_entries to count.
 */
static int zip_find_eocd(FILE *f, uint32_t *cd_off, uint16_t *num_entries)
{
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long fsize = ftell(f);
    if (fsize < 22) return 0;

    /* EOCD is within the last min(65557, fsize) bytes */
    long search_len = fsize < 65557 ? fsize : 65557;
    long search_pos = fsize - search_len;
    if (fseek(f, search_pos, SEEK_SET) != 0) return 0;

    unsigned char *buf = malloc((size_t)search_len);
    if (!buf) return 0;
    size_t nr = fread(buf, 1, (size_t)search_len, f);
    int    found = 0;
    for (long i = (long)nr - 22; i >= 0; i--) {
        if (buf[i]   == 0x50 && buf[i+1] == 0x4b &&
            buf[i+2] == 0x05 && buf[i+3] == 0x06) {
            *num_entries = u16le((unsigned char *)buf + i + 10);
            *cd_off      = u32le((unsigned char *)buf + i + 16);
            found = 1;
            break;
        }
    }
    free(buf);
    return found;
}

/*
 * Read .srt entries from the Central Directory into entries[max_entries].
 * Returns count of .srt entries found.
 */
static int zip_list_srts(const char *zip_path, ZipEntry *entries, int max_entries)
{
    FILE *f = fopen(zip_path, "rb");
    if (!f) return 0;

    uint32_t cd_off      = 0;
    uint16_t num_entries = 0;
    if (!zip_find_eocd(f, &cd_off, &num_entries)) {
        fclose(f);
        return 0;
    }

    if (fseek(f, (long)cd_off, SEEK_SET) != 0) { fclose(f); return 0; }

    int count = 0;
    unsigned char hdr[46];
    for (int i = 0; i < (int)num_entries; i++) {
        if (fread(hdr, 1, 4, f) != 4) break;
        uint32_t sig = u32le(hdr);
        if (sig != 0x02014b50u) break; /* not a CDFH signature */
        if (fread(hdr + 4, 1, 42, f) != 42) break;

        uint16_t compress   = u16le(hdr + 10);
        uint32_t comp_sz    = u32le(hdr + 20);
        uint32_t uncomp_sz  = u32le(hdr + 24);
        uint16_t fname_len  = u16le(hdr + 28);
        uint16_t extra_len  = u16le(hdr + 30);
        uint16_t comment_len= u16le(hdr + 32);
        uint32_t lhdr_off   = u32le(hdr + 42);

        char name[512] = {0};
        size_t rd = fread(name, 1, fname_len < 511 ? fname_len : 511, f);
        name[rd] = '\0';
        /* Skip extra and comment */
        fseek(f, extra_len + comment_len, SEEK_CUR);

        size_t nlen = strlen(name);
        if (nlen >= 4 && strcasecmp(name + nlen - 4, ".srt") == 0) {
            if (count < max_entries) {
                ZipEntry *e = &entries[count++];
                snprintf(e->name, sizeof(e->name), "%s", name);
                e->compress    = compress;
                e->comp_sz     = comp_sz;
                e->uncomp_sz   = uncomp_sz;
                e->local_hdr_off = lhdr_off;
            }
        }
    }
    fclose(f);
    return count;
}

/*
 * Extract zip entry e from zip_path to dest_path.
 * Handles stored (method 0) and deflated (method 8).
 */
static int zip_extract_entry(const char *zip_path, ZipEntry *e, const char *dest_path)
{
    FILE *fin = fopen(zip_path, "rb");
    if (!fin) { perror(zip_path); return 0; }

    /* Seek to local file header to find actual data offset */
    if (fseek(fin, (long)e->local_hdr_off, SEEK_SET) != 0) { fclose(fin); return 0; }
    unsigned char lhdr[30];
    if (fread(lhdr, 1, 30, fin) != 30 || u32le(lhdr) != 0x04034b50u) {
        fclose(fin); return 0;
    }
    uint16_t fname_len  = u16le(lhdr + 26);
    uint16_t extra_len  = u16le(lhdr + 28);
    fseek(fin, fname_len + extra_len, SEEK_CUR);
    /* Now at the compressed data */

    FILE *fout = fopen(dest_path, "wb");
    if (!fout) { perror(dest_path); fclose(fin); return 0; }

    int ok = 0;
    if (e->compress == 0) {
        /* Stored — direct copy */
        unsigned char buf[8192];
        uint32_t remaining = e->comp_sz;
        ok = 1;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
            size_t nr = fread(buf, 1, chunk, fin);
            if (nr == 0) { ok = 0; break; }
            if (fwrite(buf, 1, nr, fout) != nr) { ok = 0; break; }
            remaining -= (uint32_t)nr;
        }
    } else if (e->compress == 8) {
        /* Deflated — raw inflate via zlib */
        unsigned char in_buf[8192], out_buf[8192];
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, -15) != Z_OK) goto done;
        uint32_t remaining = e->comp_sz;
        ok = 1;
        int zret = Z_OK;
        while (remaining > 0 && zret != Z_STREAM_END) {
            size_t chunk = remaining < sizeof(in_buf) ? (size_t)remaining : sizeof(in_buf);
            size_t nr = fread(in_buf, 1, chunk, fin);
            if (nr == 0) { ok = 0; break; }
            remaining  -= (uint32_t)nr;
            zs.next_in  = in_buf;
            zs.avail_in = (uInt)nr;
            do {
                zs.next_out  = out_buf;
                zs.avail_out = sizeof(out_buf);
                zret = inflate(&zs, Z_NO_FLUSH);
                if (zret == Z_DATA_ERROR || zret == Z_MEM_ERROR) { ok = 0; break; }
                size_t out_bytes = sizeof(out_buf) - zs.avail_out;
                if (fwrite(out_buf, 1, out_bytes, fout) != out_bytes) { ok = 0; break; }
            } while (zs.avail_in > 0 && zret != Z_STREAM_END);
            if (!ok) break;
        }
        inflateEnd(&zs);
    } else {
        fprintf(stderr, "zip: unsupported compression method %u\n", e->compress);
    }

done:
    fclose(fin);
    fclose(fout);
    if (!ok) unlink(dest_path);
    return ok;
}

/*
 * Extract the best-matching .srt from zip_path to dest_path.
 * ep_season / ep_episode are -1 if this is a movie (no episode preference).
 * Prefers episode-matched file; falls back to largest .srt.
 */
static int extract_best_srt(const char *zip_path, const char *dest_path,
                              int ep_season, int ep_episode)
{
    ZipEntry entries[64];
    int n = zip_list_srts(zip_path, entries, 64);
    if (n == 0) {
        fprintf(stderr, "zip: no .srt files found\n");
        return 0;
    }

    int best = -1;
    ensure_re();

    if (ep_season >= 0) {
        /* Try episode-matched .srt */
        for (int i = 0; i < n; i++) {
            char norm[512];
            snprintf(norm, sizeof(norm), "%s", entries[i].name);
            normalize_separators(norm);
            int s = -1, e = -1;
            if (try_se(norm, &s, &e) && s == ep_season && e == ep_episode) {
                if (best < 0 || entries[i].uncomp_sz > entries[best].uncomp_sz)
                    best = i;
            }
        }
        if (best < 0) {
            /* No episode match — check for wrong episode or season pack */
            int largest = 0;
            for (int i = 1; i < n; i++)
                if (entries[i].uncomp_sz > entries[largest].uncomp_sz)
                    largest = i;
            /* Refuse if: multiple files (season pack) OR single file has wrong ep tag */
            int wrong_ep = 0;
            if (n == 1) {
                char norm[512];
                snprintf(norm, sizeof(norm), "%s", entries[0].name);
                normalize_separators(norm);
                int s = -1, e = -1;
                if (try_se(norm, &s, &e))
                    wrong_ep = (s != ep_season || e != ep_episode);
            }
            if (n > 1 || wrong_ep) {
                fprintf(stderr, "zip: no S%02dE%02d match (%d file(s))\n",
                        ep_season, ep_episode, n);
                return 0;
            }
            best = largest;
        }
    } else {
        /* Movie — pick largest .srt */
        best = 0;
        for (int i = 1; i < n; i++)
            if (entries[i].uncomp_sz > entries[best].uncomp_sz)
                best = i;
    }

    fprintf(stderr, "zip: extracting '%s'\n", entries[best].name);
    return zip_extract_entry(zip_path, &entries[best], dest_path);
}

/* ── Result struct ───────────────────────────────────────────────────────── */

typedef struct {
    char provider[16];
    char download_key[256];
    char display_name[84];
    char lang[8];
    int  downloads;
    int  hi;
} SubResult;

/* ── SubDL search ────────────────────────────────────────────────────────── */

typedef struct {
    SubResult *results;
    int       *count;
    int        max;
} CollectCtx;

static void subdl_collect_item(const char *item, size_t item_len, void *ud)
{
    (void)item_len;
    CollectCtx *ctx = (CollectCtx *)ud;
    if (*ctx->count >= ctx->max) return;

    SubResult *r = &ctx->results[*ctx->count];
    char url_val[256] = {0};
    if (!json_str_val(item, "url", url_val, sizeof(url_val)) || !url_val[0]) return;

    snprintf(r->provider,     sizeof(r->provider),     "subdl");
    snprintf(r->download_key, sizeof(r->download_key), "%s", url_val);
    if (!json_str_val(item, "name",     r->display_name, sizeof(r->display_name)))
        snprintf(r->display_name, sizeof(r->display_name), "Unknown");
    if (!json_str_val(item, "language", r->lang, sizeof(r->lang)))
        r->lang[0] = '\0';
    /* Lowercase lang */
    for (char *p = r->lang; *p; p++) *p = (char)tolower((unsigned char)*p);
    json_int_val(item, "downloads", &r->downloads);
    char hi_val[8] = {0};
    json_str_val(item, "hi", hi_val, sizeof(hi_val));
    r->hi = (strcmp(hi_val, "true") == 0 || strcmp(hi_val, "1") == 0) ? 1 : 0;

    (*ctx->count)++;
}

/* ── TMDB lookup ─────────────────────────────────────────────────────────── */

/* Look up a TMDB movie ID by title + year.  Returns >0 on success, -1 on failure. */
static int tmdb_movie_lookup(const char *title, int year, const char *tmdb_key)
{
    if (!tmdb_key || !tmdb_key[0]) return -1;

    char t_enc[512], key_enc[256];
    url_encode(title,    t_enc,    sizeof(t_enc));
    url_encode(tmdb_key, key_enc,  sizeof(key_enc));

    char url[1024];
    if (year > 0)
        snprintf(url, sizeof(url),
            "https://api.themoviedb.org/3/search/movie?api_key=%s&query=%s&year=%d",
            key_enc, t_enc, year);
    else
        snprintf(url, sizeof(url),
            "https://api.themoviedb.org/3/search/movie?api_key=%s&query=%s",
            key_enc, t_enc);

    MemBuf mb = {0};
    long   http_code = 0;
    CURLcode rc = http_get(url, NULL, &http_code, &mb);
    if (rc != CURLE_OK || http_code != 200) {
        fprintf(stderr, "tmdb: curl=%d http=%ld\n", rc, http_code);
        membuf_free(&mb);
        return -1;
    }

    /* Pull first result id from "results":[{"id":N,...},...] */
    int tmdb_id = -1;
    /* Find "results":[ and then the first "id": inside it */
    const char *rp = strstr(mb.data, "\"results\"");
    if (rp) {
        rp = strchr(rp, '[');
        if (rp) {
            const char *id_tok = strstr(rp, "\"id\"");
            if (id_tok) {
                id_tok += 4; /* skip "id" */
                while (*id_tok == ':' || *id_tok == ' ') id_tok++;
                if (isdigit((unsigned char)*id_tok))
                    tmdb_id = atoi(id_tok);
            }
        }
    }

    membuf_free(&mb);
    fprintf(stderr, "tmdb: id=%d for '%s' year=%d\n", tmdb_id, title, year);
    return tmdb_id;
}

static int subdl_search(const char *title, int season, int episode, int year,
                         const char *lang, const char *api_key,
                         const char *tmdb_key,
                         SubResult *results, int max_results)
{
    if (!api_key || !api_key[0]) return 0;

    char t_enc[512], lang_enc[32], key_enc[128];
    url_encode(title,   t_enc,    sizeof(t_enc));
    url_encode(api_key, key_enc,  sizeof(key_enc));
    /* SubDL expects uppercase language codes */
    char lang_up[32];
    snprintf(lang_up, sizeof(lang_up), "%s", lang);
    for (char *p = lang_up; *p; p++) *p = (char)toupper((unsigned char)*p);
    url_encode(lang_up, lang_enc, sizeof(lang_enc));

    char url[1024];
    if (season >= 0) {
        snprintf(url, sizeof(url),
            "https://api.subdl.com/api/v1/subtitles?api_key=%s&film_name=%s"
            "&languages=%s&season_number=%d&episode_number=%d&type=tv",
            key_enc, t_enc, lang_enc, season, episode);
    } else {
        /* For movies: try TMDB lookup to get a precise tmdb_id for SubDL */
        int tmdb_id = tmdb_movie_lookup(title, year, tmdb_key);
        if (tmdb_id > 0) {
            snprintf(url, sizeof(url),
                "https://api.subdl.com/api/v1/subtitles?api_key=%s&tmdb_id=%d"
                "&languages=%s&type=movie",
                key_enc, tmdb_id, lang_enc);
        } else if (year > 0) {
            snprintf(url, sizeof(url),
                "https://api.subdl.com/api/v1/subtitles?api_key=%s&film_name=%s"
                "&languages=%s&type=movie&year=%d",
                key_enc, t_enc, lang_enc, year);
        } else {
            snprintf(url, sizeof(url),
                "https://api.subdl.com/api/v1/subtitles?api_key=%s&film_name=%s"
                "&languages=%s&type=movie",
                key_enc, t_enc, lang_enc);
        }
    }

    MemBuf mb = {0};
    long   http_code = 0;
    CURLcode rc = http_get(url, NULL, &http_code, &mb);
    if (rc != CURLE_OK || http_code != 200) {
        fprintf(stderr, "subdl: curl=%d http=%ld\n", rc, http_code);
        membuf_free(&mb);
        return 0;
    }

    int        count = 0;
    CollectCtx ctx   = { results, &count, max_results };
    json_array_each(mb.data, "subtitles", subdl_collect_item, &ctx);
    membuf_free(&mb);
    fprintf(stderr, "subdl: %d results\n", count);
    return count;
}

/* ── Podnapisi search ────────────────────────────────────────────────────── */

static void podnapisi_collect_item(const char *item, size_t item_len, void *ud)
{
    (void)item_len;
    CollectCtx *ctx = (CollectCtx *)ud;
    if (*ctx->count >= ctx->max) return;

    SubResult *r = &ctx->results[*ctx->count];
    char id_val[64] = {0};
    if (!json_str_val(item, "id", id_val, sizeof(id_val)) || !id_val[0]) return;

    snprintf(r->provider,     sizeof(r->provider),     "podnapisi");
    snprintf(r->download_key, sizeof(r->download_key), "%s", id_val);
    if (!json_str_val(item, "title",    r->display_name, sizeof(r->display_name)))
        snprintf(r->display_name, sizeof(r->display_name), "Unknown");
    if (!json_str_val(item, "language", r->lang, sizeof(r->lang)))
        r->lang[0] = '\0';
    json_int_val(item, "downloads_count", &r->downloads);
    r->hi = 0;

    (*ctx->count)++;
}

static int podnapisi_search(const char *title, int season, int episode,
                              const char *lang,
                              SubResult *results, int max_results)
{
    char t_enc[512], lang_enc[32];
    url_encode(title, t_enc,    sizeof(t_enc));
    url_encode(lang,  lang_enc, sizeof(lang_enc));

    char url[1024];
    if (season >= 0) {
        snprintf(url, sizeof(url),
            "https://www.podnapisi.net/subtitles/search/?"
            "keywords=%s&language=%s&seasons=%d&episodes=%d",
            t_enc, lang_enc, season, episode);
    } else {
        snprintf(url, sizeof(url),
            "https://www.podnapisi.net/subtitles/search/?"
            "keywords=%s&language=%s",
            t_enc, lang_enc);
    }

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    MemBuf mb = {0};
    long   http_code = 0;
    CURLcode rc = http_get(url, hdrs, &http_code, &mb);
    curl_slist_free_all(hdrs);
    if (rc != CURLE_OK || http_code != 200) {
        fprintf(stderr, "podnapisi: curl=%d http=%ld\n", rc, http_code);
        membuf_free(&mb);
        return 0;
    }

    int        count = 0;
    CollectCtx ctx   = { results, &count, max_results };
    json_array_each(mb.data, "data", podnapisi_collect_item, &ctx);
    membuf_free(&mb);
    fprintf(stderr, "podnapisi: %d results\n", count);
    return count;
}

/* ── Sentinel ────────────────────────────────────────────────────────────── */

static void write_done(const char *msg)
{
    FILE *f = fopen(DONE_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

/* ── Search mode ─────────────────────────────────────────────────────────── */

static int result_cmp_dl(const void *a, const void *b)
{
    return ((const SubResult *)b)->downloads - ((const SubResult *)a)->downloads;
}

static void do_search(const char *video_path, const char *api_key, const char *lang,
                      const char *tmdb_key)
{
    unlink(RESULTS_FILE);
    unlink(DONE_FILE);

    char title[512];
    int  season = -1, episode = -1, year = -1;
    parse_filename(video_path, title, sizeof(title), &season, &episode, &year);
    fprintf(stderr, "search: title='%s' s=%d e=%d year=%d lang=%s\n",
            title, season, episode, year, lang);

    SubResult results[SUB_RESULT_MAX];
    memset(results, 0, sizeof(results));
    int count = 0;

    /* Primary: SubDL */
    if (api_key && api_key[0])
        count = subdl_search(title, season, episode, year, lang, api_key,
                              tmdb_key, results, SUB_RESULT_MAX);

    /* Fallback: Podnapisi */
    if (count == 0)
        count = podnapisi_search(title, season, episode, lang,
                                  results, SUB_RESULT_MAX);

    if (count == 0) {
        write_done("error: no subtitles found");
        return;
    }

    qsort(results, (size_t)count, sizeof(SubResult), result_cmp_dl);

    FILE *f = fopen(RESULTS_FILE, "w");
    if (!f) { write_done("error: cannot write results"); return; }
    for (int i = 0; i < count; i++) {
        SubResult *r = &results[i];
        fprintf(f, "%s|%s|%s|%s|%d|%d\n",
                r->provider, r->download_key, r->display_name,
                r->lang, r->downloads, r->hi);
    }
    fclose(f);
    fprintf(stderr, "wrote %d results to %s\n", count, RESULTS_FILE);
    write_done("ok");
}

/* ── Download mode ───────────────────────────────────────────────────────── */

static void do_download(const char *provider, const char *download_key,
                         const char *srt_dest)
{
    unlink(DONE_FILE);

    char url[512];
    if (strcmp(provider, "subdl") == 0) {
        snprintf(url, sizeof(url), "%s%s", SUBDL_DL_BASE, download_key);
    } else if (strcmp(provider, "podnapisi") == 0) {
        snprintf(url, sizeof(url),
                 "https://www.podnapisi.net/subtitles/%s/download", download_key);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "error: unknown provider '%s'", provider);
        write_done(msg);
        return;
    }

    char zip_path[256];
    snprintf(zip_path, sizeof(zip_path), "/tmp/gvu_sub_%d.zip", (int)getpid());

    fprintf(stderr, "download: %s\n", url);
    if (!http_download(url, zip_path)) {
        unlink(zip_path);
        write_done("error: download failed");
        return;
    }

    /* Parse episode from srt_dest filename to select correct entry in archive */
    char   dummy[512];
    int    ep_season = -1, ep_episode = -1, ep_year = -1;
    parse_filename(srt_dest, dummy, sizeof(dummy), &ep_season, &ep_episode, &ep_year);
    (void)ep_year;

    if (!extract_best_srt(zip_path, srt_dest, ep_season, ep_episode)) {
        unlink(zip_path);
        write_done("error: wrong episode in archive (try another result)");
        return;
    }
    unlink(zip_path);
    fprintf(stderr, "saved to %s\n", srt_dest);
    write_done("ok");
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: fetch_subs search|download ...\n");
        return 1;
    }
    ca_bundle_load();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char *mode = argv[1];
    if (strcmp(mode, "search") == 0) {
        if (argc < 5) {
            fprintf(stderr,
                    "Usage: fetch_subs search <video_path> <subdl_key> <lang> [tmdb_key]\n");
            curl_global_cleanup();
            return 1;
        }
        const char *tmdb_key = (argc >= 6) ? argv[5] : "";
        do_search(argv[2], argv[3], argv[4], tmdb_key);
    } else if (strcmp(mode, "download") == 0) {
        if (argc < 5) {
            fprintf(stderr,
                    "Usage: fetch_subs download <provider> <download_key> <srt_dest>\n");
            curl_global_cleanup();
            return 1;
        }
        do_download(argv[2], argv[3], argv[4]);
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    return 0;
}
