///*
// * ======================================================================
// *  Nabula OS v0.5  -  hosted command-line environment (single file)
// * ======================================================================
// *
// *  WHAT THIS IS
// *    A C console application that runs ON TOP OF Windows. It is NOT a
// *    kernel and NOT bootable. Nabula itself provides: the command parser,
// *    command table, history, aliases, themes, prompt and configuration.
// *    Filesystem, process, memory, disk, CPU and network commands are
// *    BACKED BY WINDOWS (Win32 API / C runtime / external Windows tools)
// *  BUILD (Dev-C++ / MinGW, C mode, no extra libraries needed):
// *    gcc Nabula_v05.c -o Nabula_v05.exe
// *
// *  STYLE NOTES
// *    - Written to compile as C89/gnu90 and later: declarations at the top
// *      of blocks, /* */ comments only, no C99-only features.
// *    - Windows-specific code is grouped in sections marked "BACKEND" so a
// *      future kernel could replace those parts.
// *
// *  DATA LOCATION
// *    %APPDATA%\NabulaOS\config.ini   (settings + aliases, plain text)
// *    %APPDATA%\NabulaOS\history.txt  (command history, plain text)
// *    If %APPDATA% is unavailable, the folder of the .exe is used.
// * ======================================================================
// */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <float.h>
#include <limits.h>

/* ---------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------- */
#define NABULA_VERSION   "0.5"
#define CMD_MAX          1024   /* max characters in one command line      */
#define MAX_ARGS         32     /* max tokens per command line             */
#define HISTORY_MAX      200
#define HIST_LEN         512
#define ALIAS_MAX        32
#define ALIAS_NAME_MAX   32
#define ALIAS_CMD_MAX    256
#define PROMPT_MAX       64
#define TREE_MAX_DEPTH   8
#define TREE_MAX_ENTRIES 2000
#define FIND_MAX_DEPTH   32
#define FIND_MAX_RESULTS 1000

#define COL_RED     0x0C
#define COL_GREEN   0x0A
#define COL_YELLOW  0x0E
#define COL_CYAN    0x0B

#define BK_NAB "Nabula internal (hosted C code, no kernel involved)"
#define BK_CRT "C runtime library on Windows"
#define BK_WIN "Windows API (Win32)"
#define BK_EXT "External Windows utility (launched as a child process)"

enum {
    CAT_CORE = 0, CAT_TERM, CAT_FS, CAT_TEXT, CAT_SYS, CAT_DEV, CAT_SESSION,
    CAT_COUNT
};

static const char *CAT_TITLES[CAT_COUNT] = {
    "Core and help", "Terminal appearance and interaction",
    "Filesystem and directories", "Text and basic utilities",
    "Process and system information", "Developer tools",
    "Session and configuration"
};
static const char *CAT_KEYS[CAT_COUNT] = {
    "core", "terminal", "files", "text", "system", "dev", "session"
};

/* ---------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------- */
typedef struct {
    char name[ALIAS_NAME_MAX];
    char command[ALIAS_CMD_MAX];
} Alias;

typedef struct {
    HANDLE hOut;
    WORD   origAttr;       /* console colours found at startup          */
    WORD   attr;           /* current text attribute                    */
    WORD   accent;         /* highlight attribute (prompt, headings)    */
    char   themeName[16];
    int    colorOverride;  /* 1 if `color` changed the theme foreground */
    char   prompt[PROMPT_MAX];
    time_t startTime;
    char   dataDir[MAX_PATH];
    char   configPath[MAX_PATH + 16];
    char   historyPath[MAX_PATH + 16];
    char   history[HISTORY_MAX][HIST_LEN];
    int    histCount;
    Alias  aliases[ALIAS_MAX];
    int    aliasCount;
    int    running;
} Shell;

typedef struct {
    char   raw[CMD_MAX];                 /* original text, trimmed        */
    char   buf[CMD_MAX + MAX_ARGS + 1];  /* unquoted tokens, NUL-separated*/
    char  *argv[MAX_ARGS];
    size_t off[MAX_ARGS];                /* start of each token in raw    */
    int    argc;
} Parsed;

typedef int (*CmdFn)(Shell *sh, Parsed *p);

typedef struct {
    const char *name;
    CmdFn       fn;
    int         category;
    const char *usage;
    const char *summary;
    const char *backend;
    const char *notes;
} Command;

typedef struct {
    const char *name;
    WORD attr;
    WORD accent;
} Theme;

/* Only two globals: needed by the Ctrl+C handler, which has no context. */
static HANDLE g_hOut = NULL;
static WORD   g_origAttr = 0x07;
static volatile LONG g_childRunning = 0;

/* Forward declarations */
static const Command *command_table(int *count);
static const Command *find_command(const char *name);
static Alias *find_alias(Shell *sh, const char *name);
static void clear_screen(Shell *sh);

/* ======================================================================
 *  SECTION 1: output helpers (colours and messages)
 * ==================================================================== */

static void set_attr(Shell *sh, WORD a)
{
    SetConsoleTextAttribute(sh->hOut, a);
}

/* Pick a foreground colour that stays readable on the current theme. */
static WORD pick_attr(const Shell *sh, WORD fg)
{
    WORD bg = (WORD)(sh->attr & 0xF0);
    if ((bg & 0x70) == 0x70) {
        fg = (WORD)(fg & 0x07);                 /* light bg: darker shade  */
    }
    if (((bg >> 4) & 0x0F) == fg) {
        fg = (WORD)((bg & 0x80) ? 0x00 : 0x0F); /* never invisible text    */
    }
    return (WORD)(bg | fg);
}

static void vcolor(Shell *sh, WORD fg, const char *prefix, const char *fmt, va_list ap)
{
    set_attr(sh, pick_attr(sh, fg));
    printf("%s", prefix);
    vprintf(fmt, ap);
    printf("\n");
    set_attr(sh, sh->attr);
}

static void msg_error(Shell *sh, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vcolor(sh, COL_RED, "Error: ", fmt, ap);
    va_end(ap);
}

static void msg_warn(Shell *sh, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vcolor(sh, COL_YELLOW, "Warning: ", fmt, ap);
    va_end(ap);
}

static void msg_ok(Shell *sh, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vcolor(sh, COL_GREEN, "", fmt, ap);
    va_end(ap);
}

static void print_accent(Shell *sh, const char *fmt, ...)
{
    va_list ap;
    set_attr(sh, sh->accent);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    set_attr(sh, sh->attr);
}

static void print_fg(Shell *sh, WORD fg, const char *text)
{
    set_attr(sh, pick_attr(sh, fg));
    printf("%s", text);
    set_attr(sh, sh->attr);
}

/* Print "Windows said no" style errors with the system message text. */
static void win_error(Shell *sh, const char *what, const char *target)
{
    DWORD code = GetLastError();
    char msg[256];
    DWORD n;
    size_t len;

    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, 0, msg, (DWORD)sizeof msg, NULL);
    if (n == 0) {
        snprintf(msg, sizeof msg, "Unknown error");
    }
    len = strlen(msg);
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r' || msg[len - 1] == ' ')) {
        msg[--len] = '\0';
    }
    msg_error(sh, "%s '%s' failed: %s (code %lu)", what, target, msg, (unsigned long)code);
    if (code == ERROR_ACCESS_DENIED) {
        printf("  Hint: permission denied. The file may be read-only, in use, or need administrator rights.\n");
    }
}

static int usage(Shell *sh, const char *name)
{
    const Command *c = find_command(name);
    if (c) {
        msg_error(sh, "Invalid or missing arguments.");
        printf("Usage: %s\n", c->usage);
    } else {
        msg_error(sh, "Invalid arguments.");
    }
    return 1;
}

/* ======================================================================
 *  SECTION 2: generic string / number helpers
 * ==================================================================== */

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        e--;
    }
    *e = '\0';
    return s;
}

static void str_lower(char *s)
{
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

static int has_control_chars(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 && c != '\t') {
            return 1;
        }
        if (c == 127) {
            return 1;
        }
    }
    return 0;
}

/* Strict integer parse: whole string must be a number within [lo,hi]. */
static int parse_long(const char *s, long *out, long lo, long hi)
{
    char *end;
    long v;
    if (s == NULL || *s == '\0') {
        return 0;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || end == s) {
        return 0;
    }
    if (v < lo || v > hi) {
        return 0;
    }
    *out = v;
    return 1;
}

/* Strict decimal parse (no hex, inf or nan). */
static int parse_double(const char *s, double *out)
{
    char *end;
    double v;
    const char *q;
    if (s == NULL || *s == '\0') {
        return 0;
    }
    for (q = s; *q; q++) {
        if (!strchr("0123456789+-.eE", *q)) {
            return 0;
        }
    }
    errno = 0;
    v = strtod(s, &end);
    if (end == s || *end != '\0' || errno == ERANGE) {
        return 0;
    }
    if (v != v || v > DBL_MAX || v < -DBL_MAX) {
        return 0;
    }
    *out = v;
    return 1;
}

static void format_size(double bytes, char *out, size_t n)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    while (bytes >= 1024.0 && i < 4) {
        bytes /= 1024.0;
        i++;
    }
    if (i == 0) {
        snprintf(out, n, "%.0f B", bytes);
    } else {
        snprintf(out, n, "%.2f %s", bytes, units[i]);
    }
}

static void format_duration(double seconds, char *out, size_t n)
{
    unsigned long total = (unsigned long)seconds;
    unsigned long d = total / 86400UL;
    unsigned long h = (total % 86400UL) / 3600UL;
    unsigned long m = (total % 3600UL) / 60UL;
    unsigned long s = total % 60UL;
    if (d > 0) {
        snprintf(out, n, "%lud %luh %lum %lus", d, h, m, s);
    } else if (h > 0) {
        snprintf(out, n, "%luh %lum %lus", h, m, s);
    } else if (m > 0) {
        snprintf(out, n, "%lum %lus", m, s);
    } else {
        snprintf(out, n, "%lus", s);
    }
}

static void format_number(double r, char *out, size_t n)
{
    if (r == 0.0) {
        r = 0.0;
    }
    if (r == floor(r) && fabs(r) < 1e15) {
        snprintf(out, n, "%.0f", r);
    } else {
        snprintf(out, n, "%.10g", r);
    }
}

static void append_str(char *out, size_t size, size_t *o, const char *s)
{
    while (*s && *o + 1 < size) {
        out[(*o)++] = *s++;
    }
    out[*o] = '\0';
}

/* Simple case-insensitive wildcard match supporting * and ?. */
static int wild_match(const char *pat, const char *s)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') {
                pat++;
            }
            if (!*pat) {
                return 1;
            }
            while (*s) {
                if (wild_match(pat, s)) {
                    return 1;
                }
                s++;
            }
            return 0;
        }
        if (!*s) {
            return 0;
        }
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) {
            return 0;
        }
        pat++;
        s++;
    }
    return *s == '\0';
}

/* ======================================================================
 *  SECTION 3: command-line parser
 * ==================================================================== */

/*
 * Splits a line into tokens. Whitespace separates tokens; double quotes
 * group text (including spaces) and are removed. Original case is kept.
 * Returns 0 on success, -1 on error (message copied to err).
 */
static int parse_line(const char *line, Parsed *p, char *err, size_t errsz)
{
    size_t i = 0, o = 0, n = strlen(line);
    int inq;

    if (n >= CMD_MAX) {
        snprintf(err, errsz, "Command too long (max %d characters).", CMD_MAX - 1);
        return -1;
    }
    memcpy(p->raw, line, n + 1);
    p->argc = 0;

    for (;;) {
        while (line[i] == ' ' || line[i] == '\t') {
            i++;
        }
        if (line[i] == '\0') {
            break;
        }
        if (p->argc >= MAX_ARGS) {
            snprintf(err, errsz, "Too many arguments (max %d).", MAX_ARGS - 1);
            return -1;
        }
        p->argv[p->argc] = p->buf + o;
        p->off[p->argc] = i;
        inq = 0;
        while (line[i] != '\0' && (inq || (line[i] != ' ' && line[i] != '\t'))) {
            if (line[i] == '"') {
                inq = !inq;
                i++;
                continue;
            }
            p->buf[o++] = line[i++];
        }
        if (inq) {
            snprintf(err, errsz, "Unterminated quote in command.");
            return -1;
        }
        p->buf[o++] = '\0';
        p->argc++;
    }
    return 0;
}

/* Raw text from token k to end of line. */
static const char *raw_rest(const Parsed *p, int k)
{
    if (k >= p->argc) {
        return "";
    }
    return p->raw + p->off[k];
}

/* Copy text from token k onward; strips one pair of surrounding quotes. */
static int text_from(const Parsed *p, int k, char *out, size_t size)
{
    const char *r = raw_rest(p, k);
    size_t n = strlen(r);
    if (n >= 2 && r[0] == '"' && r[n - 1] == '"' && memchr(r + 1, '"', n - 2) == NULL) {
        r++;
        n -= 2;
    }
    if (n >= size) {
        return -1;
    }
    memcpy(out, r, n);
    out[n] = '\0';
    return (int)n;
}

/* Read one line from stdin. Returns 0 at EOF. Flags over-long lines. */
static int read_line(char *buf, size_t size, int *tooLong)
{
    size_t len;
    int c;

    *tooLong = 0;
    if (fgets(buf, (int)size, stdin) == NULL) {
        return 0;
    }
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    } else if (len == size - 1) {
        *tooLong = 1;
        while ((c = getchar()) != '\n' && c != EOF) {
            /* discard the rest of the over-long line */
        }
    }
    if (len > 0 && buf[len - 1] == '\r') {
        buf[--len] = '\0';
    }
    return 1;
}

static int ask_yes_no(Shell *sh, const char *question)
{
    char buf[64];
    int tl;
    char *t;

    set_attr(sh, pick_attr(sh, COL_YELLOW));
    printf("%s [y/N]: ", question);
    set_attr(sh, sh->attr);
    fflush(stdout);
    if (!read_line(buf, sizeof buf, &tl)) {
        printf("\n");
        return 0;
    }
    if (tl) {
        return 0;
    }
    t = trim(buf);
    return (_stricmp(t, "y") == 0 || _stricmp(t, "yes") == 0);
}

/* ======================================================================
 *  SECTION 4: path helpers (BACKEND: Windows filesystem)
 * ==================================================================== */

static const char *path_basename(const char *p)
{
    const char *b = p, *q;
    for (q = p; *q; q++) {
        if (*q == '\\' || *q == '/') {
            b = q + 1;
        }
    }
    return b;
}

static int is_reserved_device(const char *path)
{
    const char *b = path_basename(path);
    char stem[16];
    size_t i = 0;

    while (b[i] && b[i] != '.' && i < sizeof stem - 1) {
        stem[i] = (char)toupper((unsigned char)b[i]);
        i++;
    }
    stem[i] = '\0';
    if (strcmp(stem, "CON") == 0 || strcmp(stem, "PRN") == 0 ||
        strcmp(stem, "AUX") == 0 || strcmp(stem, "NUL") == 0) {
        return 1;
    }
    if ((strncmp(stem, "COM", 3) == 0 || strncmp(stem, "LPT", 3) == 0) &&
        stem[3] >= '1' && stem[3] <= '9' && stem[4] == '\0') {
        return 1;
    }
    return 0;
}

/* Reject paths that are empty, too long, contain wildcards, control
 * characters, stray colons (alternate streams) or reserved device names. */
static int path_check(const char *p, char *why, size_t whysz)
{
    size_t i, len = strlen(p);

    if (len == 0) {
        snprintf(why, whysz, "path is empty");
        return 0;
    }
    if (len >= MAX_PATH - 16) {
        snprintf(why, whysz, "path is too long");
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 32 || strchr("*?\"<>|", c)) {
            snprintf(why, whysz, "path contains an invalid character");
            return 0;
        }
        if (c == ':') {
            if (i != 1 || !isalpha((unsigned char)p[0])) {
                snprintf(why, whysz, "':' is only allowed in a drive letter such as C:");
                return 0;
            }
        }
    }
    if (is_reserved_device(p)) {
        snprintf(why, whysz, "reserved device names (CON, NUL, COM1...) are not allowed");
        return 0;
    }
    return 1;
}

static int path_ok(Shell *sh, const char *p)
{
    char why[96];
    if (!path_check(p, why, sizeof why)) {
        msg_error(sh, "Invalid path '%s': %s.", p, why);
        return 0;
    }
    return 1;
}

/* 0 = does not exist, 1 = file, 2 = directory */
static int path_type(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    if (a == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
}

static int full_path(const char *p, char *out, DWORD size)
{
    DWORD n = GetFullPathNameA(p, size, out, NULL);
    return (n > 0 && n < size);
}

static int same_path(const char *a, const char *b)
{
    char fa[MAX_PATH], fb[MAX_PATH];
    if (!full_path(a, fa, sizeof fa) || !full_path(b, fb, sizeof fb)) {
        return 0;
    }
    return _stricmp(fa, fb) == 0;
}

static int is_drive_root(const char *p)
{
    char f[MAX_PATH];
    size_t n;
    if (!full_path(p, f, sizeof f)) {
        return 0;
    }
    n = strlen(f);
    return (n <= 3 && f[1] == ':');
}

/* If dst is an existing directory, place the source's name inside it. */
static int resolve_dest(const char *src, const char *dst, char *out, size_t size)
{
    size_t n;
    int r;
    if (path_type(dst) == 2) {
        n = strlen(dst);
        if (n > 0 && (dst[n - 1] == '\\' || dst[n - 1] == '/')) {
            r = snprintf(out, size, "%s%s", dst, path_basename(src));
        } else {
            r = snprintf(out, size, "%s\\%s", dst, path_basename(src));
        }
    } else {
        r = snprintf(out, size, "%s", dst);
    }
    return (r > 0 && (size_t)r < size && strlen(out) < MAX_PATH - 16);
}

static void filetime_str(const FILETIME *ft, char *out, size_t n)
{
    FILETIME local;
    SYSTEMTIME st;
    if (FileTimeToLocalFileTime(ft, &local) && FileTimeToSystemTime(&local, &st)) {
        snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d",
                 (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                 (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    } else {
        snprintf(out, n, "unknown");
    }
}

static double find_size(const WIN32_FIND_DATAA *fd)
{
    return (double)fd->nFileSizeHigh * 4294967296.0 + (double)fd->nFileSizeLow;
}

static int dir_is_empty(const char *dir)
{
    char pattern[MAX_PATH + 4];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int empty = 1;
    size_t len = strlen(dir);

    if (len > 0 && (dir[len - 1] == '\\' || dir[len - 1] == '/')) {
        snprintf(pattern, sizeof pattern, "%s*", dir);
    } else {
        snprintf(pattern, sizeof pattern, "%s\\*", dir);
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    do {
        if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
            empty = 0;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return empty;
}

static int find_next_valid(HANDLE h, WIN32_FIND_DATAA *fd)
{
    while (FindNextFileA(h, fd)) {
        if (strcmp(fd->cFileName, ".") != 0 && strcmp(fd->cFileName, "..") != 0) {
            return 1;
        }
    }
    return 0;
}

/* Heuristic: NUL bytes in the first 512 bytes means "not a text file". */
static int file_looks_binary(FILE *f)
{
    unsigned char buf[512];
    size_t n, i;
    int bin = 0;
    n = fread(buf, 1, sizeof buf, f);
    for (i = 0; i < n; i++) {
        if (buf[i] == 0) {
            bin = 1;
            break;
        }
    }
    rewind(f);
    return bin;
}

/* ======================================================================
 *  SECTION 5: console / registry / process helpers (BACKEND: Windows)
 * ==================================================================== */

/* Re-colour the whole buffer with attr (text stays), then set attr. */
static void restore_console_raw(HANDLE h, WORD attr)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD written, cells;
    COORD home;
    home.X = 0;
    home.Y = 0;
    if (h != NULL && GetConsoleScreenBufferInfo(h, &csbi)) {
        cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
        FillConsoleOutputAttribute(h, attr, cells, home, &written);
    }
    if (h != NULL) {
        SetConsoleTextAttribute(h, attr);
    }
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (g_childRunning) {
            return TRUE;  /* let the child process handle Ctrl+C itself */
        }
        restore_console_raw(g_hOut, g_origAttr);
        return FALSE;     /* default action: terminate Nabula */
    }
    if (type == CTRL_CLOSE_EVENT) {
        restore_console_raw(g_hOut, g_origAttr);
    }
    return FALSE;
}

static void clear_screen(Shell *sh)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD written, cells;
    COORD home;
    int i;

    home.X = 0;
    home.Y = 0;
    if (!GetConsoleScreenBufferInfo(sh->hOut, &csbi)) {
        for (i = 0; i < 50; i++) {
            printf("\n");
        }
        return;
    }
    cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    FillConsoleOutputCharacterA(sh->hOut, ' ', cells, home, &written);
    FillConsoleOutputAttribute(sh->hOut, sh->attr, cells, home, &written);
    SetConsoleCursorPosition(sh->hOut, home);
}

static int reg_read_string(HKEY root, const char *subkey, const char *value,
                           char *out, DWORD size)
{
    HKEY k;
    DWORD type = 0, sz = size;
    LONG r;

    out[0] = '\0';
    r = RegOpenKeyExA(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &k);
    if (r != ERROR_SUCCESS) {
        return 0;
    }
    r = RegQueryValueExA(k, value, NULL, &type, (LPBYTE)out, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ) {
        out[0] = '\0';
        return 0;
    }
    out[size - 1] = '\0';
    return 1;
}

static int reg_read_dword(HKEY root, const char *subkey, const char *value, DWORD *out)
{
    HKEY k;
    DWORD type = 0, sz = (DWORD)sizeof(DWORD);
    LONG r;

    r = RegOpenKeyExA(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &k);
    if (r != ERROR_SUCCESS) {
        return 0;
    }
    r = RegQueryValueExA(k, value, NULL, &type, (LPBYTE)out, &sz);
    RegCloseKey(k);
    return (r == ERROR_SUCCESS && type == REG_DWORD);
}

static void get_os_name(char *out, size_t size)
{
    char prod[128], ver[64], build[32], fixed[160];
    const char *name;
    long b;

    if (!reg_read_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                         "ProductName", prod, sizeof prod)) {
        snprintf(prod, sizeof prod, "Windows");
    }
    if (!reg_read_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                         "DisplayVersion", ver, sizeof ver)) {
        ver[0] = '\0';
    }
    if (!reg_read_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                         "CurrentBuildNumber", build, sizeof build)) {
        snprintf(build, sizeof build, "?");
    }
    name = prod;
    b = atol(build);
    /* Windows 11 still reports "Windows 10" in ProductName; build >= 22000 is Windows 11. */
    if (b >= 22000 && strncmp(prod, "Windows 10", 10) == 0) {
        snprintf(fixed, sizeof fixed, "Windows 11%s", prod + 10);
        name = fixed;
    }
    snprintf(out, size, "%s%s%s (build %s)", name, ver[0] ? " " : "", ver, build);
}

static int get_memory(MEMORYSTATUSEX *ms)
{
    ZeroMemory(ms, sizeof *ms);
    ms->dwLength = (DWORD)sizeof *ms;
    return GlobalMemoryStatusEx(ms) ? 1 : 0;
}

typedef ULONGLONG (WINAPI *GetTickCount64Fn)(void);

static double system_uptime_seconds(void)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");
    GetTickCount64Fn f;
    if (k != NULL) {
        f = (GetTickCount64Fn)GetProcAddress(k, "GetTickCount64");
        if (f != NULL) {
            return (double)f() / 1000.0;
        }
    }
    return (double)GetTickCount() / 1000.0;
}

static int find_executable(const char *name, char *out, size_t size)
{
    DWORD n = SearchPathA(NULL, name, ".exe", (DWORD)size, out, NULL);
    return (n > 0 && n < size);
}

/* Run a command line (no shell involved). Returns the exit code, -1 on failure. */
static int run_process(Shell *sh, const char *cmdline)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[4096];
    DWORD ec = 0;

    if (strlen(cmdline) >= sizeof cmd) {
        msg_error(sh, "Command line too long.");
        return -1;
    }
    strcpy(cmd, cmdline);
    ZeroMemory(&si, sizeof si);
    si.cb = (DWORD)sizeof si;
    ZeroMemory(&pi, sizeof pi);

    fflush(stdout);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        win_error(sh, "Starting", cmdline);
        return -1;
    }
    InterlockedExchange(&g_childRunning, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &ec);
    InterlockedExchange(&g_childRunning, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    set_attr(sh, sh->attr);  /* child may have changed colours */
    return (int)ec;
}

/* Run a known system tool by name with a fixed argument string. */
static int run_external(Shell *sh, const char *exe, const char *args)
{
    char full[MAX_PATH];
    char cmd[2048];

    if (!find_executable(exe, full, sizeof full)) {
        msg_error(sh, "'%s' was not found on this system (PATH search failed).", exe);
        return -1;
    }
    snprintf(cmd, sizeof cmd, "\"%s\"%s%s", full, (args && *args) ? " " : "", args ? args : "");
    return run_process(sh, cmd);
}

/* Append arg in quotes, doubling trailing backslashes so they stay literal. */
static int append_quoted(char *out, size_t size, size_t *len, const char *arg)
{
    size_t i, n = strlen(arg), trailing = 0;
    size_t need;

    while (trailing < n && arg[n - 1 - trailing] == '\\') {
        trailing++;
    }
    need = *len + 1 + 1 + n + trailing + 1 + 1;
    if (need >= size) {
        return 0;
    }
    out[(*len)++] = ' ';
    out[(*len)++] = '"';
    for (i = 0; i < n; i++) {
        out[(*len)++] = arg[i];
    }
    for (i = 0; i < trailing; i++) {
        out[(*len)++] = '\\';
    }
    out[(*len)++] = '"';
    out[*len] = '\0';
    return 1;
}

/* ======================================================================
 *  SECTION 6: themes, prompt, history, aliases, config (Nabula features)
 * ==================================================================== */

static const Theme THEMES[] = {
    { "dark",   0x07, 0x0B },
    { "light",  0xF0, 0xF1 },
    { "matrix", 0x02, 0x0A },
    { "ocean",  0x1B, 0x1E },
    { "amber",  0x06, 0x0E }
};
#define THEME_COUNT ((int)(sizeof THEMES / sizeof THEMES[0]))

static int apply_theme(Shell *sh, const char *name, int clear)
{
    int i;
    WORD bg;

    if (_stricmp(name, "default") == 0) {
        sh->attr = sh->origAttr;
        bg = (WORD)(sh->origAttr & 0xF0);
        sh->accent = (WORD)(bg | ((bg == 0x30 || bg == 0xB0) ? 0x00 : 0x0B));
        snprintf(sh->themeName, sizeof sh->themeName, "default");
    } else {
        for (i = 0; i < THEME_COUNT; i++) {
            if (_stricmp(name, THEMES[i].name) == 0) {
                sh->attr = THEMES[i].attr;
                sh->accent = THEMES[i].accent;
                snprintf(sh->themeName, sizeof sh->themeName, "%s", THEMES[i].name);
                break;
            }
        }
        if (i == THEME_COUNT) {
            return -1;
        }
    }
    sh->colorOverride = 0;
    set_attr(sh, sh->attr);
    if (clear) {
        clear_screen(sh);
    }
    return 0;
}

static int prompt_valid(const char *s)
{
    size_t n = strlen(s);
    return (n >= 1 && n < PROMPT_MAX && !has_control_chars(s));
}

static void print_prompt(Shell *sh)
{
    char out[320];
    char cwd[MAX_PATH];
    char tbuf[16];
    const char *s = sh->prompt;
    size_t o = 0;
    time_t now;
    struct tm *tmv;

    out[0] = '\0';
    while (*s) {
        if (strncmp(s, "{cwd}", 5) == 0) {
            if (GetCurrentDirectoryA((DWORD)sizeof cwd, cwd) == 0 ||
                strlen(cwd) >= sizeof cwd - 1) {
                snprintf(cwd, sizeof cwd, "?");
            }
            append_str(out, sizeof out, &o, cwd);
            s += 5;
        } else if (strncmp(s, "{time}", 6) == 0) {
            now = time(NULL);
            tmv = localtime(&now);
            if (tmv != NULL) {
                strftime(tbuf, sizeof tbuf, "%H:%M:%S", tmv);
            } else {
                snprintf(tbuf, sizeof tbuf, "--:--:--");
            }
            append_str(out, sizeof out, &o, tbuf);
            s += 6;
        } else {
            if (o + 1 < sizeof out) {
                out[o++] = *s;
                out[o] = '\0';
            }
            s++;
        }
    }
    print_accent(sh, "%s> ", out);
    fflush(stdout);
}

/* ---- history ---- */

static void history_add(Shell *sh, const char *line)
{
    if (line[0] == '\0' || has_control_chars(line)) {
        return;
    }
    if (sh->histCount > 0 && strncmp(sh->history[sh->histCount - 1], line, HIST_LEN - 1) == 0) {
        return;
    }
    if (sh->histCount >= HISTORY_MAX) {
        memmove(sh->history[0], sh->history[1], (size_t)(HISTORY_MAX - 1) * HIST_LEN);
        sh->histCount = HISTORY_MAX - 1;
    }
    snprintf(sh->history[sh->histCount], HIST_LEN, "%s", line);
    sh->histCount++;
}

static int history_save(Shell *sh)
{
    FILE *f = fopen(sh->historyPath, "w");
    int i;
    if (f == NULL) {
        return -1;
    }
    for (i = 0; i < sh->histCount; i++) {
        fprintf(f, "%s\n", sh->history[i]);
    }
    return (fclose(f) == 0) ? 0 : -1;
}

/* Returns number of lines loaded, or -1 if the file cannot be opened. */
static int history_load(Shell *sh)
{
    FILE *f = fopen(sh->historyPath, "r");
    char line[HIST_LEN + 2];
    size_t len;
    int loaded = 0, c;
    char *t;

    if (f == NULL) {
        return -1;
    }
    while (fgets(line, (int)sizeof line, f) != NULL) {
        len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        } else if (!feof(f)) {
            while ((c = fgetc(f)) != '\n' && c != EOF) {
                /* skip rest of over-long line */
            }
            continue;
        }
        t = trim(line);
        if (*t == '\0' || strlen(t) >= HIST_LEN || has_control_chars(t)) {
            continue;
        }
        history_add(sh, t);
        loaded++;
    }
    fclose(f);
    return loaded;
}

/* ---- aliases ---- */

static Alias *find_alias(Shell *sh, const char *name)
{
    int i;
    for (i = 0; i < sh->aliasCount; i++) {
        if (_stricmp(sh->aliases[i].name, name) == 0) {
            return &sh->aliases[i];
        }
    }
    return NULL;
}

static int alias_name_valid(const char *n)
{
    size_t i, len = strlen(n);
    if (len == 0 || len >= ALIAS_NAME_MAX) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (!isalnum((unsigned char)n[i]) && n[i] != '_' && n[i] != '-') {
            return 0;
        }
    }
    return 1;
}

/* Aliases may only expand to built-in Nabula commands (never to shell text). */
static int alias_set(Shell *sh, const char *name, const char *command, char *err, size_t errsz)
{
    char lname[ALIAS_NAME_MAX];
    char first[64];
    size_t i = 0;
    Alias *a;

    if (!alias_name_valid(name)) {
        snprintf(err, errsz, "alias name must be 1-%d letters, digits, '_' or '-'", ALIAS_NAME_MAX - 1);
        return -1;
    }
    snprintf(lname, sizeof lname, "%s", name);
    str_lower(lname);
    if (find_command(lname) != NULL) {
        snprintf(err, errsz, "'%s' is already a built-in command", lname);
        return -1;
    }
    if (command[0] == '\0' || strlen(command) >= ALIAS_CMD_MAX || has_control_chars(command)) {
        snprintf(err, errsz, "alias command must be 1-%d printable characters", ALIAS_CMD_MAX - 1);
        return -1;
    }
    while (command[i] && !isspace((unsigned char)command[i]) && i < sizeof first - 1) {
        first[i] = (char)tolower((unsigned char)command[i]);
        i++;
    }
    first[i] = '\0';
    if (find_command(first) == NULL) {
        snprintf(err, errsz, "alias must begin with a built-in Nabula command (got '%s')", first);
        return -1;
    }
    a = find_alias(sh, lname);
    if (a == NULL) {
        if (sh->aliasCount >= ALIAS_MAX) {
            snprintf(err, errsz, "alias table is full (%d)", ALIAS_MAX);
            return -1;
        }
        a = &sh->aliases[sh->aliasCount++];
        snprintf(a->name, sizeof a->name, "%s", lname);
    }
    snprintf(a->command, sizeof a->command, "%s", command);
    return 0;
}

/* ---- configuration (plain key=value text; never executed) ---- */

static void set_defaults(Shell *sh)
{
    snprintf(sh->prompt, sizeof sh->prompt, "nabula {cwd}");
    sh->aliasCount = 0;
    apply_theme(sh, "default", 0);
}

static int write_config(Shell *sh)
{
    FILE *f = fopen(sh->configPath, "w");
    int i;
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "# Nabula OS v%s configuration (plain text, edited by 'save')\n", NABULA_VERSION);
    fprintf(f, "# Values are data only; nothing here is executed at load time.\n");
    fprintf(f, "theme=%s\n", sh->themeName);
    fprintf(f, "prompt=%s\n", sh->prompt);
    for (i = 0; i < sh->aliasCount; i++) {
        fprintf(f, "alias.%s=%s\n", sh->aliases[i].name, sh->aliases[i].command);
    }
    return (fclose(f) == 0) ? 0 : -1;
}

/* Returns -1 if the file cannot be opened, else number of rejected lines. */
static int load_config(Shell *sh, int verbose)
{
    FILE *f = fopen(sh->configPath, "r");
    char line[1100], err[128];
    char *t, *eq, *key, *val;
    size_t len;
    int lines = 0, bad = 0, c;

    if (f == NULL) {
        if (verbose) {
            msg_warn(sh, "No config file found at %s", sh->configPath);
        }
        return -1;
    }
    while (lines < 500 && fgets(line, (int)sizeof line, f) != NULL) {
        lines++;
        len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        } else if (!feof(f)) {
            while ((c = fgetc(f)) != '\n' && c != EOF) {
                /* skip over-long line */
            }
            msg_warn(sh, "config line %d is too long; ignored.", lines);
            bad++;
            continue;
        }
        t = trim(line);
        if (*t == '\0' || *t == '#') {
            continue;
        }
        eq = strchr(t, '=');
        if (eq == NULL) {
            msg_warn(sh, "config line %d has no '='; ignored.", lines);
            bad++;
            continue;
        }
        *eq = '\0';
        key = trim(t);
        val = trim(eq + 1);
        str_lower(key);
        if (strcmp(key, "theme") == 0) {
            if (apply_theme(sh, val, 0) != 0) {
                msg_warn(sh, "config line %d: unknown theme '%s'.", lines, val);
                bad++;
            }
        } else if (strcmp(key, "prompt") == 0) {
            if (prompt_valid(val)) {
                snprintf(sh->prompt, sizeof sh->prompt, "%s", val);
            } else {
                msg_warn(sh, "config line %d: invalid prompt.", lines);
                bad++;
            }
        } else if (strncmp(key, "alias.", 6) == 0) {
            if (alias_set(sh, key + 6, val, err, sizeof err) != 0) {
                msg_warn(sh, "config line %d: %s.", lines, err);
                bad++;
            }
        } else {
            msg_warn(sh, "config line %d: unknown key '%s'.", lines, key);
            bad++;
        }
    }
    fclose(f);
    return bad;
}

static void init_paths(Shell *sh)
{
    char *appdata = getenv("APPDATA");
    char exe[MAX_PATH];
    char *slash;

    sh->dataDir[0] = '\0';
    if (appdata != NULL && strlen(appdata) + 16 < MAX_PATH) {
        snprintf(sh->dataDir, sizeof sh->dataDir, "%s\\NabulaOS", appdata);
        CreateDirectoryA(sh->dataDir, NULL);
        if (path_type(sh->dataDir) != 2) {
            sh->dataDir[0] = '\0';
        }
    }
    if (sh->dataDir[0] == '\0') {
        if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe) > 0) {
            slash = strrchr(exe, '\\');
            if (slash != NULL) {
                *slash = '\0';
            }
            snprintf(sh->dataDir, sizeof sh->dataDir, "%s", exe);
        } else {
            snprintf(sh->dataDir, sizeof sh->dataDir, ".");
        }
    }
    snprintf(sh->configPath, sizeof sh->configPath, "%s\\config.ini", sh->dataDir);
    snprintf(sh->historyPath, sizeof sh->historyPath, "%s\\history.txt", sh->dataDir);
}

/* ======================================================================
 *  SECTION 7: A. core and help commands
 * ==================================================================== */

static const char *BANNER_LINES[5] = {
    " _   _     _    ____  _   _ _          _    ",
    "| \\ | |   / \\  | __ )| | | | |        / \\   ",
    "|  \\| |  / _ \\ |  _ \\| | | | |       / _ \\  ",
    "| |\\  | / ___ \\| |_) | |_| | |___   / ___ \\ ",
    "|_| \\_|/_/   \\_\\____/ \\___/|_____| /_/   \\_\\"
};

static void print_banner(Shell *sh)
{
    int i;
    for (i = 0; i < 5; i++) {
        print_accent(sh, "%s\n", BANNER_LINES[i]);
    }
    print_accent(sh, "  Nabula OS v%s", NABULA_VERSION);
    printf("  -  hosted CLI environment running on Windows\n");
}

static void print_command_detail(Shell *sh, const Command *c, int full)
{
    print_accent(sh, "%s", c->name);
    printf(" - %s\n", c->summary);
    printf("  Usage:   %s\n", c->usage);
    if (full) {
        printf("  Section: %s\n", CAT_TITLES[c->category]);
        printf("  Backend: %s\n", c->backend);
    }
    if (c->notes && c->notes[0]) {
        printf("  Notes:   %s\n", c->notes);
    }
}

static void help_category(Shell *sh, int cat)
{
    int n, i;
    const Command *t = command_table(&n);
    print_accent(sh, "\n%s\n", CAT_TITLES[cat]);
    for (i = 0; i < n; i++) {
        if (t[i].category == cat) {
            printf("  %-12s %s\n", t[i].name, t[i].summary);
        }
    }
}

static int cmd_help(Shell *sh, Parsed *p)
{
    int i;
    const Command *c;
    Alias *a;
    char name[64];

    if (p->argc > 2) {
        return usage(sh, "help");
    }
    if (p->argc == 1) {
        for (i = 0; i < CAT_COUNT; i++) {
            help_category(sh, i);
        }
        printf("\nType 'help <command>' for usage, 'help <section>' for one section\n");
        printf("(sections: core, terminal, files, text, system, dev, session),\n");
        printf("or 'man <command>' for the full manual entry.\n");
        return 0;
    }
    snprintf(name, sizeof name, "%s", p->argv[1]);
    str_lower(name);
    for (i = 0; i < CAT_COUNT; i++) {
        if (strcmp(name, CAT_KEYS[i]) == 0) {
            help_category(sh, i);
            if (i == CAT_DEV) {
                printf("\nDeveloper tools only DETECT and run external programs; none are bundled.\n");
                printf("Use 'devcheck' to see which are installed.\n");
            }
            return 0;
        }
    }
    c = find_command(name);
    if (c != NULL) {
        print_command_detail(sh, c, 0);
        return 0;
    }
    a = find_alias(sh, name);
    if (a != NULL) {
        printf("'%s' is an alias for: %s\n", a->name, a->command);
        return 0;
    }
    msg_error(sh, "No help for '%s'. Type 'help' for the list.", p->argv[1]);
    return 1;
}

static int cmd_man(Shell *sh, Parsed *p)
{
    const Command *c;
    char name[64];
    Alias *a;

    if (p->argc != 2) {
        return usage(sh, "man");
    }
    snprintf(name, sizeof name, "%s", p->argv[1]);
    str_lower(name);
    a = find_alias(sh, name);
    if (a != NULL) {
        printf("'%s' is an alias for: %s\n", a->name, a->command);
        return 0;
    }
    c = find_command(name);
    if (c == NULL) {
        msg_error(sh, "No manual entry for '%s'.", p->argv[1]);
        return 1;
    }
    print_command_detail(sh, c, 1);
    return 0;
}

static int cmd_info(Shell *sh, Parsed *p)
{
    char os[200];
    (void)p;
    get_os_name(os, sizeof os);
    print_accent(sh, "Nabula OS v%s\n", NABULA_VERSION);
    printf("Type:        hosted command-line environment (single C program)\n");
    printf("Host OS:     %s\n", os);
    printf("Kernel:      none. Nabula has no kernel, scheduler, virtual memory or boot code.\n");
    printf("Build:       %s %s\n", __DATE__, __TIME__);
    printf("Data folder: %s\n", sh->dataDir);
    printf("\nNabula-native: command parser, history, aliases, themes, prompt, config.\n");
    printf("Windows-backed: filesystem, process, memory, disk, CPU and network commands.\n");
    printf("Use 'man <command>' to see which backend a command uses.\n");
    return 0;
}

static int cmd_version(Shell *sh, Parsed *p)
{
    (void)p;
    print_accent(sh, "Nabula OS v%s", NABULA_VERSION);
    printf(" (built %s %s) - hosted CLI, not a kernel\n", __DATE__, __TIME__);
    return 0;
}

static int cmd_about(Shell *sh, Parsed *p)
{
    (void)p;
    print_banner(sh);
    printf("\nNabula OS is a learning project: a C command shell with its own parser,\n");
    printf("command table, history, aliases, themes and configuration.\n");
    printf("Right now it runs as a normal Windows console program. Windows-specific\n");
    printf("code lives in sections marked BACKEND so a real kernel could replace it\n");
    printf("later. Nothing here boots, schedules tasks or manages hardware.\n");
    return 0;
}

static int cmd_clear(Shell *sh, Parsed *p)
{
    (void)p;
    clear_screen(sh);
    return 0;
}

static int cmd_exit(Shell *sh, Parsed *p)
{
    (void)p;
    sh->running = 0;
    return 0;
}

static int cmd_date(Shell *sh, Parsed *p)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[64];
    (void)p;
    if (t == NULL || strftime(buf, sizeof buf, "%A, %B %d, %Y", t) == 0) {
        msg_error(sh, "Could not read the system date.");
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}

static int cmd_time(Shell *sh, Parsed *p)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[64];
    (void)p;
    if (t == NULL || strftime(buf, sizeof buf, "%H:%M:%S", t) == 0) {
        msg_error(sh, "Could not read the system time.");
        return 1;
    }
    printf("%s (local time)\n", buf);
    return 0;
}

static int cmd_uptime(Shell *sh, Parsed *p)
{
    char a[48], b[48];
    (void)p;
    format_duration(difftime(time(NULL), sh->startTime), a, sizeof a);
    format_duration(system_uptime_seconds(), b, sizeof b);
    printf("Nabula session uptime: %s\n", a);
    printf("Windows uptime:        %s (reported by the host)\n", b);
    return 0;
}

static int cmd_whoami(Shell *sh, Parsed *p)
{
    char user[256];
    DWORD n = (DWORD)sizeof user;
    (void)p;
    if (!GetUserNameA(user, &n)) {
        win_error(sh, "Reading", "user name");
        return 1;
    }
    printf("%s\n", user);
    return 0;
}

static int cmd_hostname(Shell *sh, Parsed *p)
{
    char name[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD n = (DWORD)sizeof name;
    (void)p;
    if (!GetComputerNameA(name, &n)) {
        win_error(sh, "Reading", "computer name");
        return 1;
    }
    printf("%s\n", name);
    return 0;
}

static int cmd_status(Shell *sh, Parsed *p)
{
    char cwd[MAX_PATH], up[48];
    (void)p;
    if (GetCurrentDirectoryA((DWORD)sizeof cwd, cwd) == 0) {
        snprintf(cwd, sizeof cwd, "?");
    }
    format_duration(difftime(time(NULL), sh->startTime), up, sizeof up);
    print_accent(sh, "Nabula OS v%s status\n", NABULA_VERSION);
    printf("Session uptime:   %s\n", up);
    printf("Working folder:   %s\n", cwd);
    printf("Theme:            %s%s\n", sh->themeName, sh->colorOverride ? " (custom foreground)" : "");
    printf("Prompt:           %s\n", sh->prompt);
    printf("History entries:  %d / %d\n", sh->histCount, HISTORY_MAX);
    printf("Aliases:          %d / %d\n", sh->aliasCount, ALIAS_MAX);
    printf("Data folder:      %s\n", sh->dataDir);
    printf("Mode:             hosted on Windows (no Nabula kernel)\n");
    return 0;
}

static const char *NEO_LOGO[7] = {
    "       .  *  .        ",
    "    *    ____    .    ",
    "   .   _/    \\_  *    ",
    "      /  N  A  \\      ",
    "   *  \\_  OS  _/ .    ",
    "    .   \\____/   *    ",
    "       *  .  *        "
};

static int cmd_neofetch(Shell *sh, Parsed *p)
{
    char rows[11][180];
    char os[200], cpu[128], user[256], host[MAX_COMPUTERNAME_LENGTH + 2];
    char up1[48], up2[48], m1[32], m2[32];
    DWORD n1 = (DWORD)sizeof user, n2 = (DWORD)sizeof host;
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    int i;
    (void)p;

    if (!GetUserNameA(user, &n1)) {
        snprintf(user, sizeof user, "user");
    }
    if (!GetComputerNameA(host, &n2)) {
        snprintf(host, sizeof host, "host");
    }
    get_os_name(os, sizeof os);
    if (!reg_read_string(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                         "ProcessorNameString", cpu, sizeof cpu)) {
        snprintf(cpu, sizeof cpu, "unknown CPU");
    }
    GetSystemInfo(&si);
    format_duration(difftime(time(NULL), sh->startTime), up1, sizeof up1);
    format_duration(system_uptime_seconds(), up2, sizeof up2);

    snprintf(rows[0], sizeof rows[0], "%s@%s", user, host);
    snprintf(rows[1], sizeof rows[1], "----------------------");
    snprintf(rows[2], sizeof rows[2], "OS:       %s", os);
    snprintf(rows[3], sizeof rows[3], "Shell:    Nabula OS v%s (hosted CLI)", NABULA_VERSION);
    snprintf(rows[4], sizeof rows[4], "Kernel:   Windows NT (host); Nabula has none");
    snprintf(rows[5], sizeof rows[5], "CPU:      %s (%lu logical)", trim(cpu), (unsigned long)si.dwNumberOfProcessors);
    if (get_memory(&ms)) {
        format_size((double)(ms.ullTotalPhys - ms.ullAvailPhys), m1, sizeof m1);
        format_size((double)ms.ullTotalPhys, m2, sizeof m2);
        snprintf(rows[6], sizeof rows[6], "Memory:   %s / %s", m1, m2);
    } else {
        snprintf(rows[6], sizeof rows[6], "Memory:   unavailable");
    }
    snprintf(rows[7], sizeof rows[7], "Uptime:   session %s, Windows %s", up1, up2);
    snprintf(rows[8], sizeof rows[8], "Theme:    %s", sh->themeName);
    snprintf(rows[9], sizeof rows[9], "Terminal: Windows console");
    snprintf(rows[10], sizeof rows[10], "Data:     %s", sh->dataDir);

    printf("\n");
    for (i = 0; i < 11; i++) {
        if (i < 7) {
            print_accent(sh, "%s", NEO_LOGO[i]);
        } else {
            printf("%s", "                      ");
        }
        printf("  %s\n", rows[i]);
    }
    printf("\n");
    return 0;
}

static int cmd_history(Shell *sh, Parsed *p)
{
    int i, n;
    if (p->argc == 1) {
        if (sh->histCount == 0) {
            printf("History is empty.\n");
        }
        for (i = 0; i < sh->histCount; i++) {
            printf("%4d  %s\n", i + 1, sh->history[i]);
        }
        return 0;
    }
    if (p->argc != 2) {
        return usage(sh, "history");
    }
    if (_stricmp(p->argv[1], "clear") == 0) {
        sh->histCount = 0;
        msg_ok(sh, "History cleared (in memory). Run 'history save' to overwrite the saved file.");
        return 0;
    }
    if (_stricmp(p->argv[1], "save") == 0) {
        if (history_save(sh) != 0) {
            msg_error(sh, "Could not write %s (%s).", sh->historyPath, strerror(errno));
            return 1;
        }
        msg_ok(sh, "Saved %d entries to %s", sh->histCount, sh->historyPath);
        return 0;
    }
    if (_stricmp(p->argv[1], "load") == 0) {
        n = history_load(sh);
        if (n < 0) {
            msg_error(sh, "Could not read %s (%s).", sh->historyPath, strerror(errno));
            return 1;
        }
        msg_ok(sh, "Loaded %d entries from %s", n, sh->historyPath);
        return 0;
    }
    return usage(sh, "history");
}

/* ======================================================================
 *  SECTION 8: B. terminal appearance and interaction
 * ==================================================================== */

static int cmd_theme(Shell *sh, Parsed *p)
{
    int i;
    if (p->argc == 1) {
        printf("Current theme: %s\nAvailable: default", sh->themeName);
        for (i = 0; i < THEME_COUNT; i++) {
            printf(", %s", THEMES[i].name);
        }
        printf("\n");
        return 0;
    }
    if (p->argc != 2) {
        return usage(sh, "theme");
    }
    if (apply_theme(sh, p->argv[1], 1) != 0) {
        msg_error(sh, "Unknown theme '%s'. Try: default, dark, light, matrix, ocean, amber.", p->argv[1]);
        return 1;
    }
    msg_ok(sh, "Theme set to %s.", sh->themeName);
    return 0;
}

typedef struct { const char *name; WORD value; } ColorName;

static const ColorName COLOR_NAMES[] = {
    { "black", 0x0 }, { "blue", 0x1 }, { "green", 0x2 }, { "cyan", 0x3 },
    { "aqua", 0x3 }, { "red", 0x4 }, { "magenta", 0x5 }, { "purple", 0x5 },
    { "yellow", 0x6 }, { "white", 0x7 }, { "gray", 0x8 }, { "grey", 0x8 },
    { "brightblue", 0x9 }, { "brightgreen", 0xA }, { "brightcyan", 0xB },
    { "brightred", 0xC }, { "brightmagenta", 0xD }, { "brightyellow", 0xE },
    { "brightwhite", 0xF }
};
#define COLOR_COUNT ((int)(sizeof COLOR_NAMES / sizeof COLOR_NAMES[0]))

static int cmd_color(Shell *sh, Parsed *p)
{
    int i;
    WORD bg;
    if (p->argc == 1) {
        printf("Colors:");
        for (i = 0; i < COLOR_COUNT; i++) {
            printf(" %s", COLOR_NAMES[i].name);
        }
        printf("\nSets the text color only; the theme background is kept.\n");
        return 0;
    }
    if (p->argc != 2) {
        return usage(sh, "color");
    }
    bg = (WORD)(sh->attr & 0xF0);
    for (i = 0; i < COLOR_COUNT; i++) {
        if (_stricmp(p->argv[1], COLOR_NAMES[i].name) == 0) {
            if (COLOR_NAMES[i].value == (WORD)(bg >> 4)) {
                msg_error(sh, "That color matches the background and would be invisible.");
                return 1;
            }
            sh->attr = (WORD)(bg | COLOR_NAMES[i].value);
            sh->colorOverride = 1;
            set_attr(sh, sh->attr);
            msg_ok(sh, "Text color set to %s (session only; 'theme' resets it).", COLOR_NAMES[i].name);
            return 0;
        }
    }
    msg_error(sh, "Unknown color '%s'. Type 'color' for the list.", p->argv[1]);
    return 1;
}

static int cmd_prompt(Shell *sh, Parsed *p)
{
    char text[CMD_MAX];
    if (p->argc == 1) {
        printf("Current prompt: %s\n", sh->prompt);
        printf("Tokens: {cwd} = working folder, {time} = clock. 'prompt reset' restores the default.\n");
        return 0;
    }
    if (p->argc == 2 && _stricmp(p->argv[1], "reset") == 0) {
        snprintf(sh->prompt, sizeof sh->prompt, "nabula {cwd}");
        msg_ok(sh, "Prompt reset.");
        return 0;
    }
    if (text_from(p, 1, text, sizeof text) < 0 || !prompt_valid(text)) {
        msg_error(sh, "Prompt must be 1-%d printable characters.", PROMPT_MAX - 1);
        return 1;
    }
    snprintf(sh->prompt, sizeof sh->prompt, "%s", text);
    msg_ok(sh, "Prompt updated.");
    return 0;
}

static int cmd_echo(Shell *sh, Parsed *p)
{
    char text[CMD_MAX];
    (void)sh;
    if (text_from(p, 1, text, sizeof text) < 0) {
        return 1;
    }
    printf("%s\n", text);
    return 0;
}

static int cmd_repeat(Shell *sh, Parsed *p)
{
    long count, i;
    char text[CMD_MAX];
    if (p->argc < 3 || !parse_long(p->argv[1], &count, 1, 100)) {
        msg_error(sh, "Count must be a whole number from 1 to 100.");
        printf("Usage: repeat <count> <text>\n");
        return 1;
    }
    if (text_from(p, 2, text, sizeof text) < 0) {
        return 1;
    }
    for (i = 0; i < count; i++) {
        printf("%s\n", text);
    }
    return 0;
}

static int cmd_banner(Shell *sh, Parsed *p)
{
    (void)p;
    print_banner(sh);
    return 0;
}

static int cmd_beep(Shell *sh, Parsed *p)
{
    (void)sh;
    (void)p;
    if (!Beep(880, 150)) {
        printf("\a");
    }
    return 0;
}

static int cmd_pause(Shell *sh, Parsed *p)
{
    char buf[64];
    int tl;
    (void)sh;
    (void)p;
    printf("Press Enter to continue . . . ");
    fflush(stdout);
    read_line(buf, sizeof buf, &tl);
    return 0;
}

static int cmd_confirm(Shell *sh, Parsed *p)
{
    char text[CMD_MAX];
    if (p->argc == 1) {
        snprintf(text, sizeof text, "Are you sure?");
    } else if (text_from(p, 1, text, sizeof text) < 0) {
        return 1;
    }
    if (ask_yes_no(sh, text)) {
        msg_ok(sh, "Confirmed (yes).");
    } else {
        printf("Declined (no).\n");
    }
    return 0;
}

/* ======================================================================
 *  SECTION 9: C. filesystem commands (BACKEND: Windows filesystem)
 * ==================================================================== */

static int cmd_pwd(Shell *sh, Parsed *p)
{
    char cwd[MAX_PATH];
    DWORD n;
    (void)p;
    n = GetCurrentDirectoryA((DWORD)sizeof cwd, cwd);
    if (n == 0 || n >= sizeof cwd) {
        win_error(sh, "Reading", "current directory");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

static int list_directory(Shell *sh, const char *path, int detailed)
{
    char pattern[MAX_PATH + 4], tbuf[32], sbuf[32];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    unsigned long nFiles = 0, nDirs = 0;
    double total = 0.0, size;
    size_t len;
    int isDir;

    if (!path_ok(sh, path)) {
        return 1;
    }
    if (path_type(path) != 2) {
        msg_error(sh, "'%s' is not a directory or does not exist.", path);
        return 1;
    }
    len = strlen(path);
    if (len + 3 >= sizeof pattern) {
        msg_error(sh, "Path is too long.");
        return 1;
    }
    if (len > 0 && (path[len - 1] == '\\' || path[len - 1] == '/')) {
        snprintf(pattern, sizeof pattern, "%s*", path);
    } else {
        snprintf(pattern, sizeof pattern, "%s\\*", path);
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        win_error(sh, "Listing", path);
        return 1;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        size = find_size(&fd);
        if (isDir) {
            nDirs++;
        } else {
            nFiles++;
            total += size;
        }
        if (detailed) {
            filetime_str(&fd.ftLastWriteTime, tbuf, sizeof tbuf);
            if (isDir) {
                snprintf(sbuf, sizeof sbuf, "<DIR>");
            } else {
                snprintf(sbuf, sizeof sbuf, "%.0f", size);
            }
            printf("%s  %12s  ", tbuf, sbuf);
        } else {
            printf("  ");
        }
        if (isDir) {
            set_attr(sh, pick_attr(sh, COL_CYAN));
            printf("%s%s", fd.cFileName, detailed ? "" : "\\");
            set_attr(sh, sh->attr);
            printf("\n");
        } else if (detailed) {
            printf("%s\n", fd.cFileName);
        } else {
            format_size(size, sbuf, sizeof sbuf);
            printf("%-40s %s\n", fd.cFileName, sbuf);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    format_size(total, sbuf, sizeof sbuf);
    printf("%lu folder(s), %lu file(s), %s in files\n", nDirs, nFiles, sbuf);
    return 0;
}

static int cmd_ls(Shell *sh, Parsed *p)
{
    if (p->argc > 2) {
        return usage(sh, "ls");
    }
    return list_directory(sh, p->argc == 2 ? p->argv[1] : ".", 0);
}

static int cmd_dir(Shell *sh, Parsed *p)
{
    if (p->argc > 2) {
        return usage(sh, "dir");
    }
    return list_directory(sh, p->argc == 2 ? p->argv[1] : ".", 1);
}

static int cmd_cd(Shell *sh, Parsed *p)
{
    if (p->argc == 1) {
        return cmd_pwd(sh, p);
    }
    if (p->argc > 2) {
        msg_error(sh, "Too many arguments. Put paths with spaces in quotes: cd \"My Folder\"");
        return 1;
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    if (path_type(p->argv[1]) != 2) {
        msg_error(sh, "'%s' is not a directory or does not exist.", p->argv[1]);
        return 1;
    }
    if (!SetCurrentDirectoryA(p->argv[1])) {
        win_error(sh, "Changing to", p->argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_mkdir(Shell *sh, Parsed *p)
{
    if (p->argc != 2) {
        return usage(sh, "mkdir");
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    if (path_type(p->argv[1]) != 0) {
        msg_error(sh, "'%s' already exists.", p->argv[1]);
        return 1;
    }
    if (!CreateDirectoryA(p->argv[1], NULL)) {
        win_error(sh, "Creating", p->argv[1]);
        return 1;
    }
    msg_ok(sh, "Created folder '%s'.", p->argv[1]);
    return 0;
}

static int cmd_rmdir(Shell *sh, Parsed *p)
{
    const char *d;
    char q[MAX_PATH + 64];
    if (p->argc != 2) {
        return usage(sh, "rmdir");
    }
    d = p->argv[1];
    if (!path_ok(sh, d)) {
        return 1;
    }
    if (strcmp(d, ".") == 0 || strcmp(d, "..") == 0 || is_drive_root(d)) {
        msg_error(sh, "Refusing to remove '%s'.", d);
        return 1;
    }
    if (path_type(d) != 2) {
        msg_error(sh, "'%s' is not a directory or does not exist.", d);
        return 1;
    }
    if (!dir_is_empty(d)) {
        msg_error(sh, "'%s' is not empty. Nabula never deletes folders recursively.", d);
        return 1;
    }
    snprintf(q, sizeof q, "Remove empty folder '%s'?", d);
    if (!ask_yes_no(sh, q)) {
        printf("Cancelled.\n");
        return 0;
    }
    if (!RemoveDirectoryA(d)) {
        win_error(sh, "Removing", d);
        return 1;
    }
    msg_ok(sh, "Removed folder '%s'.", d);
    return 0;
}

static int cmd_touch(Shell *sh, Parsed *p)
{
    HANDLE h;
    FILETIME ft;
    int existed;
    if (p->argc != 2) {
        return usage(sh, "touch");
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    existed = path_type(p->argv[1]);
    if (existed == 2) {
        msg_error(sh, "'%s' is a directory.", p->argv[1]);
        return 1;
    }
    h = CreateFileA(p->argv[1], FILE_WRITE_ATTRIBUTES | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        win_error(sh, "Touching", p->argv[1]);
        return 1;
    }
    GetSystemTimeAsFileTime(&ft);
    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    msg_ok(sh, existed ? "Updated modified time of '%s' (contents untouched)." : "Created empty file '%s'.",
           p->argv[1]);
    return 0;
}

static int cmd_cat(Shell *sh, Parsed *p)
{
    FILE *f;
    int c, last = '\n';
    if (p->argc != 2) {
        return usage(sh, p->argv[0]);
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    if (path_type(p->argv[1]) == 2) {
        msg_error(sh, "'%s' is a directory.", p->argv[1]);
        return 1;
    }
    f = fopen(p->argv[1], "r");
    if (f == NULL) {
        msg_error(sh, "Cannot open '%s': %s.", p->argv[1], strerror(errno));
        return 1;
    }
    if (file_looks_binary(f)) {
        msg_error(sh, "'%s' looks like a binary file; not printing it.", p->argv[1]);
        fclose(f);
        return 1;
    }
    while ((c = fgetc(f)) != EOF) {
        putchar(c);
        last = c;
    }
    if (ferror(f)) {
        msg_error(sh, "Read error on '%s'.", p->argv[1]);
    }
    if (last != '\n') {
        putchar('\n');
    }
    fclose(f);
    return 0;
}

/* Shared by `write` and `append`. */
static int write_or_append(Shell *sh, Parsed *p, int append)
{
    FILE *f;
    char line[CMD_MAX + 2], q[MAX_PATH + 64];
    int tl, existed;
    unsigned long lines = 0;

    if (p->argc != 2) {
        return usage(sh, p->argv[0]);
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    existed = path_type(p->argv[1]);
    if (existed == 2) {
        msg_error(sh, "'%s' is a directory.", p->argv[1]);
        return 1;
    }
    if (!append && existed == 1) {
        snprintf(q, sizeof q, "'%s' exists. Overwrite it (old contents are lost)?", p->argv[1]);
        if (!ask_yes_no(sh, q)) {
            printf("Cancelled.\n");
            return 0;
        }
    }
    f = fopen(p->argv[1], append ? "a" : "w");
    if (f == NULL) {
        msg_error(sh, "Cannot open '%s': %s.", p->argv[1], strerror(errno));
        return 1;
    }
    printf("Enter text. A line containing only '.' (or end of input) finishes.\n");
    for (;;) {
        printf("| ");
        fflush(stdout);
        if (!read_line(line, sizeof line, &tl)) {
            printf("\n");
            break;
        }
        if (tl) {
            msg_warn(sh, "Line was too long and was truncated.");
        }
        if (strcmp(line, ".") == 0) {
            break;
        }
        fprintf(f, "%s\n", line);
        lines++;
    }
    if (fclose(f) != 0) {
        msg_error(sh, "Error while closing '%s'.", p->argv[1]);
        return 1;
    }
    msg_ok(sh, "%s %lu line(s) %s '%s'.", append ? "Appended" : "Wrote", lines,
           append ? "to" : "into", p->argv[1]);
    return 0;
}

static int cmd_write(Shell *sh, Parsed *p)
{
    return write_or_append(sh, p, 0);
}

static int cmd_append(Shell *sh, Parsed *p)
{
    return write_or_append(sh, p, 1);
}

static int cmd_copy(Shell *sh, Parsed *p)
{
    char dest[MAX_PATH], q[2 * MAX_PATH];
    int dt;
    if (p->argc != 3) {
        return usage(sh, "copy");
    }
    if (!path_ok(sh, p->argv[1]) || !path_ok(sh, p->argv[2])) {
        return 1;
    }
    if (path_type(p->argv[1]) != 1) {
        msg_error(sh, "Source '%s' must be an existing file.", p->argv[1]);
        return 1;
    }
    if (!resolve_dest(p->argv[1], p->argv[2], dest, sizeof dest)) {
        msg_error(sh, "Destination path is invalid or too long.");
        return 1;
    }
    if (same_path(p->argv[1], dest)) {
        msg_error(sh, "Source and destination are the same file.");
        return 1;
    }
    dt = path_type(dest);
    if (dt == 2) {
        msg_error(sh, "Destination '%s' is a directory.", dest);
        return 1;
    }
    if (dt == 1) {
        snprintf(q, sizeof q, "'%s' exists. Overwrite it?", dest);
        if (!ask_yes_no(sh, q)) {
            printf("Cancelled.\n");
            return 0;
        }
    }
    if (!CopyFileA(p->argv[1], dest, FALSE)) {
        win_error(sh, "Copying", p->argv[1]);
        return 1;
    }
    msg_ok(sh, "Copied to '%s'.", dest);
    return 0;
}

static int cmd_move(Shell *sh, Parsed *p)
{
    char dest[MAX_PATH], q[2 * MAX_PATH];
    int st, dt;
    DWORD flags = MOVEFILE_COPY_ALLOWED;
    if (p->argc != 3) {
        return usage(sh, "move");
    }
    if (!path_ok(sh, p->argv[1]) || !path_ok(sh, p->argv[2])) {
        return 1;
    }
    st = path_type(p->argv[1]);
    if (st == 0) {
        msg_error(sh, "Source '%s' does not exist.", p->argv[1]);
        return 1;
    }
    if (is_drive_root(p->argv[1])) {
        msg_error(sh, "Refusing to move a drive root.");
        return 1;
    }
    if (!resolve_dest(p->argv[1], p->argv[2], dest, sizeof dest)) {
        msg_error(sh, "Destination path is invalid or too long.");
        return 1;
    }
    if (same_path(p->argv[1], dest)) {
        msg_error(sh, "Source and destination are the same.");
        return 1;
    }
    dt = path_type(dest);
    if (dt == 2) {
        msg_error(sh, "Destination '%s' is an existing directory.", dest);
        return 1;
    }
    if (dt == 1) {
        if (st == 2) {
            msg_error(sh, "Cannot replace file '%s' with a directory.", dest);
            return 1;
        }
        snprintf(q, sizeof q, "'%s' exists. Overwrite it?", dest);
        if (!ask_yes_no(sh, q)) {
            printf("Cancelled.\n");
            return 0;
        }
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (!MoveFileExA(p->argv[1], dest, flags)) {
        win_error(sh, "Moving", p->argv[1]);
        return 1;
    }
    msg_ok(sh, "Moved to '%s'.", dest);
    return 0;
}

static int cmd_rename(Shell *sh, Parsed *p)
{
    char dest[MAX_PATH], dirpart[MAX_PATH];
    const char *base;
    size_t dlen;
    if (p->argc != 3) {
        return usage(sh, "rename");
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    if (strpbrk(p->argv[2], "\\/:") != NULL || strcmp(p->argv[2], ".") == 0 ||
        strcmp(p->argv[2], "..") == 0 || !path_check(p->argv[2], dest, sizeof dest)) {
        msg_error(sh, "New name must be a plain name (no folders). Use 'move' to relocate.");
        return 1;
    }
    if (path_type(p->argv[1]) == 0) {
        msg_error(sh, "'%s' does not exist.", p->argv[1]);
        return 1;
    }
    base = path_basename(p->argv[1]);
    dlen = (size_t)(base - p->argv[1]);
    if (dlen >= sizeof dirpart) {
        msg_error(sh, "Path is too long.");
        return 1;
    }
    memcpy(dirpart, p->argv[1], dlen);
    dirpart[dlen] = '\0';
    if (snprintf(dest, sizeof dest, "%s%s", dirpart, p->argv[2]) >= (int)sizeof dest) {
        msg_error(sh, "Resulting path is too long.");
        return 1;
    }
    /* Allow case-only renames (same file, different spelling); block overwrites. */
    if (path_type(dest) != 0 && !(same_path(p->argv[1], dest) && strcmp(base, p->argv[2]) != 0)) {
        msg_error(sh, "'%s' already exists. 'rename' never overwrites; use 'move' for that.", p->argv[2]);
        return 1;
    }
    if (!MoveFileExA(p->argv[1], dest, 0)) {
        win_error(sh, "Renaming", p->argv[1]);
        return 1;
    }
    msg_ok(sh, "Renamed to '%s'.", p->argv[2]);
    return 0;
}

static int cmd_delete(Shell *sh, Parsed *p)
{
    char q[MAX_PATH + 96];
    if (p->argc != 2) {
        return usage(sh, "delete");
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    if (path_type(p->argv[1]) != 1) {
        msg_error(sh, "'%s' is not an existing file (folders are never deleted by 'delete').", p->argv[1]);
        return 1;
    }
    snprintf(q, sizeof q, "PERMANENTLY delete '%s'? This bypasses the Recycle Bin.", p->argv[1]);
    if (!ask_yes_no(sh, q)) {
        printf("Cancelled.\n");
        return 0;
    }
    if (!DeleteFileA(p->argv[1])) {
        win_error(sh, "Deleting", p->argv[1]);
        return 1;
    }
    msg_ok(sh, "Deleted '%s'.", p->argv[1]);
    return 0;
}

static int cmd_exists(Shell *sh, Parsed *p)
{
    int t;
    if (p->argc != 2) {
        return usage(sh, "exists");
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    t = path_type(p->argv[1]);
    if (t == 0) {
        printf("'%s' does not exist.\n", p->argv[1]);
    } else {
        printf("'%s' exists (%s).\n", p->argv[1], t == 2 ? "directory" : "file");
    }
    return 0;
}

static int cmd_filesize(Shell *sh, Parsed *p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    double sz;
    char buf[32];
    if (p->argc != 2) {
        return usage(sh, "filesize");
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    if (!GetFileAttributesExA(p->argv[1], GetFileExInfoStandard, &d)) {
        win_error(sh, "Reading", p->argv[1]);
        return 1;
    }
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        msg_error(sh, "'%s' is a directory; filesize works on files.", p->argv[1]);
        return 1;
    }
    sz = (double)d.nFileSizeHigh * 4294967296.0 + (double)d.nFileSizeLow;
    format_size(sz, buf, sizeof buf);
    printf("%.0f bytes (%s)\n", sz, buf);
    return 0;
}

typedef struct {
    unsigned long dirs, files, printed;
    int truncated;
} TreeStats;

static void tree_walk(Shell *sh, const char *dir, const char *prefix, int depth, TreeStats *st)
{
    char pattern[MAX_PATH + 4], child[MAX_PATH], newPrefix[96];
    WIN32_FIND_DATAA fd, cur;
    HANDLE h;
    int have, haveNext, isLast, isDir;
    size_t len = strlen(dir);

    if (len + 3 >= sizeof pattern) {
        return;
    }
    if (len > 0 && (dir[len - 1] == '\\' || dir[len - 1] == '/')) {
        snprintf(pattern, sizeof pattern, "%s*", dir);
    } else {
        snprintf(pattern, sizeof pattern, "%s\\*", dir);
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    have = 1;
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
        have = find_next_valid(h, &fd);
    }
    while (have && !st->truncated) {
        cur = fd;
        haveNext = find_next_valid(h, &fd);
        isLast = !haveNext;
        isDir = (cur.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;

        printf("%s%s", prefix, isLast ? "\\-- " : "+-- ");
        if (isDir) {
            print_fg(sh, COL_CYAN, cur.cFileName);
            printf("\n");
            st->dirs++;
        } else {
            printf("%s\n", cur.cFileName);
            st->files++;
        }
        st->printed++;
        if (st->printed >= TREE_MAX_ENTRIES) {
            st->truncated = 1;
            break;
        }
        if (isDir && !(cur.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            depth < TREE_MAX_DEPTH && strlen(prefix) + 4 < sizeof newPrefix) {
            if (snprintf(child, sizeof child, "%s%s%s", dir,
                         (len > 0 && (dir[len - 1] == '\\' || dir[len - 1] == '/')) ? "" : "\\",
                         cur.cFileName) < (int)sizeof child) {
                snprintf(newPrefix, sizeof newPrefix, "%s%s", prefix, isLast ? "    " : "|   ");
                tree_walk(sh, child, newPrefix, depth + 1, st);
            }
        }
        have = haveNext;
    }
    FindClose(h);
}

static int cmd_tree(Shell *sh, Parsed *p)
{
    const char *root;
    TreeStats st;
    if (p->argc > 2) {
        return usage(sh, "tree");
    }
    root = (p->argc == 2) ? p->argv[1] : ".";
    if (!path_ok(sh, root)) {
        return 1;
    }
    if (path_type(root) != 2) {
        msg_error(sh, "'%s' is not a directory or does not exist.", root);
        return 1;
    }
    memset(&st, 0, sizeof st);
    print_accent(sh, "%s\n", root);
    tree_walk(sh, root, "", 0, &st);
    printf("\n%lu folder(s), %lu file(s)\n", st.dirs, st.files);
    if (st.truncated) {
        msg_warn(sh, "Output stopped after %d entries.", TREE_MAX_ENTRIES);
    }
    printf("(Depth limit %d; junctions/symlinks are listed but not followed.)\n", TREE_MAX_DEPTH);
    return 0;
}

static void find_walk(Shell *sh, const char *dir, const char *pat, int depth, unsigned long *hits)
{
    char pattern[MAX_PATH + 4], child[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    size_t len = strlen(dir);
    int sep;

    if (len + 3 >= sizeof pattern || *hits >= FIND_MAX_RESULTS) {
        return;
    }
    sep = (len > 0 && (dir[len - 1] == '\\' || dir[len - 1] == '/'));
    snprintf(pattern, sizeof pattern, sep ? "%s*" : "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if (snprintf(child, sizeof child, sep ? "%s%s" : "%s\\%s", dir, fd.cFileName) >= (int)sizeof child) {
            continue;
        }
        if (wild_match(pat, fd.cFileName)) {
            printf("%s\n", child);
            (*hits)++;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            depth < FIND_MAX_DEPTH && *hits < FIND_MAX_RESULTS) {
            find_walk(sh, child, pat, depth + 1, hits);
        }
    } while (*hits < FIND_MAX_RESULTS && FindNextFileA(h, &fd));
    FindClose(h);
}

static int cmd_find(Shell *sh, Parsed *p)
{
    unsigned long hits = 0;
    if (p->argc != 3) {
        return usage(sh, "find");
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    if (path_type(p->argv[1]) != 2) {
        msg_error(sh, "'%s' is not a directory or does not exist.", p->argv[1]);
        return 1;
    }
    if (strlen(p->argv[2]) > 64 || strpbrk(p->argv[2], "\\/:") != NULL || has_control_chars(p->argv[2])) {
        msg_error(sh, "Name pattern must be a plain file name (max 64 chars; * and ? allowed).");
        return 1;
    }
    find_walk(sh, p->argv[1], p->argv[2], 0, &hits);
    printf("%lu match(es).\n", hits);
    if (hits >= FIND_MAX_RESULTS) {
        msg_warn(sh, "Stopped after %d results.", FIND_MAX_RESULTS);
    }
    return 0;
}

static int cmd_stat(Shell *sh, Parsed *p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    char full[MAX_PATH], tbuf[32], sbuf[32];
    DWORD a;
    double sz;
    if (p->argc != 2) {
        return usage(sh, "stat");
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    if (!GetFileAttributesExA(p->argv[1], GetFileExInfoStandard, &d)) {
        win_error(sh, "Reading", p->argv[1]);
        return 1;
    }
    if (!full_path(p->argv[1], full, sizeof full)) {
        snprintf(full, sizeof full, "%s", p->argv[1]);
    }
    a = d.dwFileAttributes;
    sz = (double)d.nFileSizeHigh * 4294967296.0 + (double)d.nFileSizeLow;
    printf("Path:       %s\n", full);
    printf("Type:       %s%s\n", (a & FILE_ATTRIBUTE_DIRECTORY) ? "directory" : "file",
           (a & FILE_ATTRIBUTE_REPARSE_POINT) ? " (reparse point / link)" : "");
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        format_size(sz, sbuf, sizeof sbuf);
        printf("Size:       %.0f bytes (%s)\n", sz, sbuf);
    }
    filetime_str(&d.ftCreationTime, tbuf, sizeof tbuf);
    printf("Created:    %s\n", tbuf);
    filetime_str(&d.ftLastWriteTime, tbuf, sizeof tbuf);
    printf("Modified:   %s\n", tbuf);
    filetime_str(&d.ftLastAccessTime, tbuf, sizeof tbuf);
    printf("Accessed:   %s\n", tbuf);
    printf("Attributes: %s%s%s%s%s%s%s\n",
           (a & FILE_ATTRIBUTE_READONLY) ? "ReadOnly " : "",
           (a & FILE_ATTRIBUTE_HIDDEN) ? "Hidden " : "",
           (a & FILE_ATTRIBUTE_SYSTEM) ? "System " : "",
           (a & FILE_ATTRIBUTE_ARCHIVE) ? "Archive " : "",
           (a & FILE_ATTRIBUTE_COMPRESSED) ? "Compressed " : "",
           (a & FILE_ATTRIBUTE_ENCRYPTED) ? "Encrypted " : "",
           (a & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                 FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED)) ? "" : "(none)");
    return 0;
}

static int count_lines(FILE *f, unsigned long *lines)
{
    int c, last = '\n';
    unsigned long n = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            n++;
        }
        last = c;
    }
    if (last != '\n') {
        n++;
    }
    *lines = n;
    return ferror(f) ? -1 : 0;
}

/* Opens a text file for head/tail with all validation. Returns NULL on error. */
static FILE *open_text_file(Shell *sh, const char *path)
{
    FILE *f;
    if (!path_ok(sh, path)) {
        return NULL;
    }
    if (path_type(path) != 1) {
        msg_error(sh, "'%s' is not an existing file.", path);
        return NULL;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        msg_error(sh, "Cannot open '%s': %s.", path, strerror(errno));
        return NULL;
    }
    if (file_looks_binary(f)) {
        msg_error(sh, "'%s' looks like a binary file.", path);
        fclose(f);
        return NULL;
    }
    return f;
}

static int parse_line_count(Shell *sh, Parsed *p, long *n)
{
    *n = 10;
    if (p->argc == 3 && !parse_long(p->argv[2], n, 1, 1000000L)) {
        msg_error(sh, "Line count must be a whole number from 1 to 1000000.");
        return 0;
    }
    return 1;
}

static int cmd_head(Shell *sh, Parsed *p)
{
    FILE *f;
    long n;
    unsigned long cur = 0;
    int c, last = '\n';
    if (p->argc < 2 || p->argc > 3) {
        return usage(sh, "head");
    }
    if (!parse_line_count(sh, p, &n)) {
        return 1;
    }
    f = open_text_file(sh, p->argv[1]);
    if (f == NULL) {
        return 1;
    }
    while (cur < (unsigned long)n && (c = fgetc(f)) != EOF) {
        putchar(c);
        last = c;
        if (c == '\n') {
            cur++;
        }
    }
    if (last != '\n') {
        putchar('\n');
    }
    fclose(f);
    return 0;
}

static int cmd_tail(Shell *sh, Parsed *p)
{
    FILE *f;
    long n;
    unsigned long total = 0, skip, cur = 0;
    int c, last = '\n';
    if (p->argc < 2 || p->argc > 3) {
        return usage(sh, "tail");
    }
    if (!parse_line_count(sh, p, &n)) {
        return 1;
    }
    f = open_text_file(sh, p->argv[1]);
    if (f == NULL) {
        return 1;
    }
    if (count_lines(f, &total) != 0) {
        msg_error(sh, "Read error on '%s'.", p->argv[1]);
        fclose(f);
        return 1;
    }
    rewind(f);
    skip = (total > (unsigned long)n) ? total - (unsigned long)n : 0;
    while ((c = fgetc(f)) != EOF) {
        if (cur >= skip) {
            putchar(c);
            last = c;
        }
        if (c == '\n') {
            cur++;
        }
    }
    if (last != '\n') {
        putchar('\n');
    }
    fclose(f);
    return 0;
}

/* ======================================================================
 *  SECTION 10: D. text and basic utilities
 * ==================================================================== */

static int cmd_calc(Shell *sh, Parsed *p)
{
    double a, b, r;
    long ia, ib;
    char op, buf[64];

    if (p->argc != 4 || strlen(p->argv[2]) != 1) {
        return usage(sh, "calc");
    }
    op = p->argv[2][0];
    if (op == '%') {
        if (!parse_long(p->argv[1], &ia, -2000000000L, 2000000000L) ||
            !parse_long(p->argv[3], &ib, -2000000000L, 2000000000L)) {
            msg_error(sh, "Modulo needs two whole numbers between -2000000000 and 2000000000.");
            return 1;
        }
        if (ib == 0) {
            msg_error(sh, "Modulo by zero is undefined.");
            return 1;
        }
        printf("%ld %% %ld = %ld  (result takes the sign of the first number)\n", ia, ib, ia % ib);
        return 0;
    }
    if (!parse_double(p->argv[1], &a) || !parse_double(p->argv[3], &b)) {
        msg_error(sh, "Operands must be plain decimal numbers (for example 12, -3.5, 2e3).");
        return 1;
    }
    switch (op) {
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    case '*': r = a * b; break;
    case '/':
        if (b == 0.0) {
            msg_error(sh, "Division by zero is undefined.");
            return 1;
        }
        r = a / b;
        break;
    default:
        msg_error(sh, "Unsupported operator '%c'. Use + - * / %%.", op);
        return 1;
    }
    if (r != r || r > DBL_MAX || r < -DBL_MAX) {
        msg_error(sh, "Result is out of range.");
        return 1;
    }
    format_number(r, buf, sizeof buf);
    printf("%s %c %s = %s\n", p->argv[1], op, p->argv[3], buf);
    return 0;
}

static int cmd_strlen(Shell *sh, Parsed *p)
{
    char text[CMD_MAX];
    if (p->argc < 2 || text_from(p, 1, text, sizeof text) < 0) {
        return usage(sh, "strlen");
    }
    printf("%lu byte(s)\n", (unsigned long)strlen(text));
    return 0;
}

static int cmd_reverse(Shell *sh, Parsed *p)
{
    char text[CMD_MAX];
    size_t i, n;
    char t;
    if (p->argc < 2 || text_from(p, 1, text, sizeof text) < 0) {
        return usage(sh, "reverse");
    }
    n = strlen(text);
    for (i = 0; i < n / 2; i++) {
        t = text[i];
        text[i] = text[n - 1 - i];
        text[n - 1 - i] = t;
    }
    printf("%s\n", text);
    return 0;
}

static int cmd_upper(Shell *sh, Parsed *p)
{
    char text[CMD_MAX];
    char *s;
    if (p->argc < 2 || text_from(p, 1, text, sizeof text) < 0) {
        return usage(sh, "upper");
    }
    for (s = text; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
    printf("%s\n", text);
    return 0;
}

static int cmd_lower(Shell *sh, Parsed *p)
{
    char text[CMD_MAX];
    if (p->argc < 2 || text_from(p, 1, text, sizeof text) < 0) {
        return usage(sh, "lower");
    }
    str_lower(text);
    printf("%s\n", text);
    return 0;
}

static int cmd_count(Shell *sh, Parsed *p)
{
    FILE *f;
    unsigned long lines = 0, words = 0, bytes = 0;
    int c, inWord = 0, last = '\n';
    if (p->argc != 2) {
        return usage(sh, "count");
    }
    if (!path_ok(sh, p->argv[1])) {
        return 1;
    }
    if (path_type(p->argv[1]) != 1) {
        msg_error(sh, "'%s' is not an existing file.", p->argv[1]);
        return 1;
    }
    f = fopen(p->argv[1], "rb");
    if (f == NULL) {
        msg_error(sh, "Cannot open '%s': %s.", p->argv[1], strerror(errno));
        return 1;
    }
    while ((c = fgetc(f)) != EOF) {
        bytes++;
        if (c == '\n') {
            lines++;
        }
        if (isspace((unsigned char)c)) {
            inWord = 0;
        } else if (!inWord) {
            inWord = 1;
            words++;
        }
        last = c;
    }
    if (ferror(f)) {
        msg_error(sh, "Read error on '%s'.", p->argv[1]);
        fclose(f);
        return 1;
    }
    fclose(f);
    if (bytes > 0 && last != '\n') {
        lines++;
    }
    printf("Lines: %lu   Words: %lu   Bytes: %lu\n", lines, words, bytes);
    return 0;
}

static unsigned long rand30(void)
{
    return (((unsigned long)rand() << 15) | (unsigned long)rand()) & 0x3FFFFFFFUL;
}

static int cmd_random(Shell *sh, Parsed *p)
{
    long lo = 1, hi = 100;
    unsigned long range;
    if (p->argc > 3) {
        return usage(sh, "random");
    }
    if (p->argc == 2 && !parse_long(p->argv[1], &hi, -500000000L, 500000000L)) {
        msg_error(sh, "Maximum must be a whole number within +/-500000000.");
        return 1;
    }
    if (p->argc == 3 && (!parse_long(p->argv[1], &lo, -500000000L, 500000000L) ||
                         !parse_long(p->argv[2], &hi, -500000000L, 500000000L))) {
        msg_error(sh, "Min and max must be whole numbers within +/-500000000.");
        return 1;
    }
    if (lo > hi) {
        msg_error(sh, "Minimum (%ld) is greater than maximum (%ld).", lo, hi);
        return 1;
    }
    range = (unsigned long)(hi - lo) + 1UL;
    printf("%ld\n", lo + (long)(rand30() % range));
    return 0;
}

static int cmd_timestamp(Shell *sh, Parsed *p)
{
    time_t now = time(NULL);
    struct tm *g, *l;
    char gb[48], lb[48];
    (void)sh;
    (void)p;
    g = gmtime(&now);
    if (g != NULL) {
        strftime(gb, sizeof gb, "%Y-%m-%d %H:%M:%S", g);
    } else {
        snprintf(gb, sizeof gb, "unavailable");
    }
    l = localtime(&now);
    if (l != NULL) {
        strftime(lb, sizeof lb, "%Y-%m-%d %H:%M:%S", l);
    } else {
        snprintf(lb, sizeof lb, "unavailable");
    }
    printf("Unix time: %.0f\nUTC:       %s\nLocal:     %s\n", (double)now, gb, lb);
    return 0;
}

static int cmd_env(Shell *sh, Parsed *p)
{
    char *block, *s;
    unsigned long n = 0;
    (void)p;
    block = GetEnvironmentStringsA();
    if (block == NULL) {
        win_error(sh, "Reading", "environment");
        return 1;
    }
    for (s = block; *s; s += strlen(s) + 1) {
        if (s[0] == '=') {
            continue;  /* hidden per-drive entries such as =C: */
        }
        printf("%s\n", s);
        n++;
    }
    FreeEnvironmentStringsA(block);
    printf("(%lu variables)\n", n);
    return 0;
}

static int cmd_getenv(Shell *sh, Parsed *p)
{
    DWORD n;
    char *val;
    if (p->argc != 2) {
        return usage(sh, "getenv");
    }
    if (strchr(p->argv[1], '=') != NULL || strlen(p->argv[1]) > 255) {
        msg_error(sh, "Invalid variable name.");
        return 1;
    }
    n = GetEnvironmentVariableA(p->argv[1], NULL, 0);
    if (n == 0) {
        printf("%s is not set.\n", p->argv[1]);
        return 0;
    }
    val = (char *)malloc((size_t)n + 1);
    if (val == NULL) {
        msg_error(sh, "Out of memory.");
        return 1;
    }
    if (GetEnvironmentVariableA(p->argv[1], val, n + 1) == 0) {
        val[0] = '\0';
    }
    printf("%s=%s\n", p->argv[1], val);
    free(val);
    return 0;
}

static int cmd_which(Shell *sh, Parsed *p)
{
    char name[64], full[MAX_PATH];
    const Command *c;
    Alias *a;
    if (p->argc != 2) {
        return usage(sh, "which");
    }
    snprintf(name, sizeof name, "%s", p->argv[1]);
    str_lower(name);
    a = find_alias(sh, name);
    if (a != NULL) {
        printf("%s: alias for '%s'\n", a->name, a->command);
        return 0;
    }
    c = find_command(name);
    if (c != NULL) {
        printf("%s: Nabula built-in command (%s)\n", c->name, CAT_TITLES[c->category]);
        return 0;
    }
    if (strpbrk(p->argv[1], "\\/:") == NULL && path_check(p->argv[1], full, sizeof full) &&
        find_executable(p->argv[1], full, sizeof full)) {
        printf("%s: external program at %s\n", p->argv[1], full);
        return 0;
    }
    printf("%s: not found (not a built-in, alias, or program on PATH)\n", p->argv[1]);
    return 1;
}

/* ======================================================================
 *  SECTION 11: E. process and system information (BACKEND: Windows)
 * ==================================================================== */

static const char *arch_name(WORD a)
{
    switch (a) {
    case 0:  return "x86";
    case 5:  return "ARM";
    case 9:  return "x64 (AMD64)";
    case 12: return "ARM64";
    default: return "unknown";
    }
}

static int cmd_sysinfo(Shell *sh, Parsed *p)
{
    char os[200], host[MAX_COMPUTERNAME_LENGTH + 2], user[256], m[32];
    DWORD hn = (DWORD)sizeof host, un = (DWORD)sizeof user;
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    (void)p;
    get_os_name(os, sizeof os);
    if (!GetComputerNameA(host, &hn)) {
        snprintf(host, sizeof host, "unknown");
    }
    if (!GetUserNameA(user, &un)) {
        snprintf(user, sizeof user, "unknown");
    }
    GetNativeSystemInfo(&si);
    print_accent(sh, "System information (reported by Windows)\n");
    printf("OS:            %s\n", os);
    printf("Computer:      %s\n", host);
    printf("User:          %s\n", user);
    printf("Architecture:  %s\n", arch_name(si.wProcessorArchitecture));
    printf("Logical CPUs:  %lu\n", (unsigned long)si.dwNumberOfProcessors);
    printf("Page size:     %lu bytes\n", (unsigned long)si.dwPageSize);
    if (get_memory(&ms)) {
        format_size((double)ms.ullTotalPhys, m, sizeof m);
        printf("Physical RAM:  %s\n", m);
    }
    printf("Note: these values come from Windows, not from a Nabula kernel.\n");
    return 0;
}

static int cmd_meminfo(Shell *sh, Parsed *p)
{
    MEMORYSTATUSEX ms;
    char a[32], b[32], c[32], d[32];
    (void)p;
    if (!get_memory(&ms)) {
        win_error(sh, "Reading", "memory status");
        return 1;
    }
    print_accent(sh, "Memory information (reported by Windows)\n");
    format_size((double)ms.ullTotalPhys, a, sizeof a);
    format_size((double)ms.ullAvailPhys, b, sizeof b);
    printf("Physical RAM:   %s total, %s available (%lu%% in use)\n", a, b, (unsigned long)ms.dwMemoryLoad);
    format_size((double)ms.ullTotalPageFile, c, sizeof c);
    format_size((double)ms.ullAvailPageFile, d, sizeof d);
    printf("Commit limit:   %s total, %s available (RAM + page file)\n", c, d);
    format_size((double)ms.ullTotalVirtual, a, sizeof a);
    format_size((double)ms.ullAvailVirtual, b, sizeof b);
    printf("Address space:  %s total, %s free for this Nabula process\n", a, b);
    printf("Note: Nabula does not manage memory; Windows does.\n");
    return 0;
}

static int cmd_cpuinfo(Shell *sh, Parsed *p)
{
    char name[128], vendor[64];
    DWORD mhz = 0;
    SYSTEM_INFO si;
    (void)p;
    GetNativeSystemInfo(&si);
    print_accent(sh, "CPU information (reported by Windows)\n");
    if (reg_read_string(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                        "ProcessorNameString", name, sizeof name)) {
        printf("Model:         %s\n", trim(name));
    }
    if (reg_read_string(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                        "VendorIdentifier", vendor, sizeof vendor)) {
        printf("Vendor:        %s\n", vendor);
    }
    if (reg_read_dword(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz", &mhz)) {
        printf("Nominal speed: %lu MHz\n", (unsigned long)mhz);
    }
    printf("Architecture:  %s\n", arch_name(si.wProcessorArchitecture));
    printf("Logical CPUs:  %lu\n", (unsigned long)si.dwNumberOfProcessors);
    printf("Level/Rev:     %u / 0x%04X\n", (unsigned)si.wProcessorLevel, (unsigned)si.wProcessorRevision);
    return 0;
}

static int cmd_diskinfo(Shell *sh, Parsed *p)
{
    DWORD mask = GetLogicalDrives();
    int i;
    char root[4], label[64], fs[32], t[32], f[32], u[32];
    const char *kind;
    UINT type;
    ULARGE_INTEGER avail, total, freeB;
    (void)p;
    if (mask == 0) {
        win_error(sh, "Reading", "drive list");
        return 1;
    }
    print_accent(sh, "Drives (reported by Windows)\n");
    printf("%-6s %-10s %-14s %-8s %10s %10s %10s\n", "Drive", "Type", "Label", "FS", "Total", "Used", "Free");
    for (i = 0; i < 26; i++) {
        if (!(mask & (1UL << i))) {
            continue;
        }
        root[0] = (char)('A' + i);
        root[1] = ':';
        root[2] = '\\';
        root[3] = '\0';
        type = GetDriveTypeA(root);
        switch (type) {
        case DRIVE_FIXED:     kind = "fixed"; break;
        case DRIVE_REMOVABLE: kind = "removable"; break;
        case DRIVE_REMOTE:    kind = "network"; break;
        case DRIVE_CDROM:     kind = "cd/dvd"; break;
        case DRIVE_RAMDISK:   kind = "ramdisk"; break;
        default:              kind = "unknown"; break;
        }
        label[0] = '\0';
        fs[0] = '\0';
        GetVolumeInformationA(root, label, (DWORD)sizeof label, NULL, NULL, NULL, fs, (DWORD)sizeof fs);
        if (GetDiskFreeSpaceExA(root, &avail, &total, &freeB)) {
            format_size((double)total.QuadPart, t, sizeof t);
            format_size((double)(total.QuadPart - freeB.QuadPart), u, sizeof u);
            format_size((double)freeB.QuadPart, f, sizeof f);
            printf("%-6s %-10s %-14.14s %-8s %10s %10s %10s\n", root, kind, label, fs, t, u, f);
        } else {
            printf("%-6s %-10s %-14s %-8s %10s %10s %10s\n", root, kind, "-", "-", "n/a", "n/a", "n/a");
        }
    }
    return 0;
}

static int cmd_processes(Shell *sh, Parsed *p)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    unsigned long n = 0;
    (void)p;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        win_error(sh, "Taking", "process snapshot");
        return 1;
    }
    ZeroMemory(&pe, sizeof pe);
    pe.dwSize = (DWORD)sizeof pe;
    if (!Process32First(snap, &pe)) {
        win_error(sh, "Reading", "process list");
        CloseHandle(snap);
        return 1;
    }
    print_accent(sh, "%7s %7s %8s  %s\n", "PID", "PPID", "THREADS", "NAME");
    do {
        printf("%7lu %7lu %8lu  %s\n", (unsigned long)pe.th32ProcessID,
               (unsigned long)pe.th32ParentProcessID, (unsigned long)pe.cntThreads, pe.szExeFile);
        n++;
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    printf("%lu process(es). Listed via the Windows ToolHelp API; Nabula has no scheduler.\n", n);
    return 0;
}

static int cmd_tasklist(Shell *sh, Parsed *p)
{
    if (p->argc != 1) {
        return usage(sh, "tasklist");
    }
    return run_external(sh, "tasklist", "") < 0 ? 1 : 0;
}

static int host_valid(const char *h)
{
    size_t i, n = strlen(h);
    if (n == 0 || n > 253 || h[0] == '-') {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!isalnum((unsigned char)h[i]) && h[i] != '.' && h[i] != '-' && h[i] != ':') {
            return 0;
        }
    }
    return 1;
}

static int cmd_ping(Shell *sh, Parsed *p)
{
    long count = 4;
    char args[400];
    if (p->argc < 2 || p->argc > 3) {
        return usage(sh, "ping");
    }
    if (!host_valid(p->argv[1])) {
        msg_error(sh, "Invalid host. Use a name or IP address (letters, digits, '.', '-', ':').");
        return 1;
    }
    if (p->argc == 3 && !parse_long(p->argv[2], &count, 1, 10)) {
        msg_error(sh, "Count must be a whole number from 1 to 10.");
        return 1;
    }
    snprintf(args, sizeof args, "-n %ld %s", count, p->argv[1]);
    return run_external(sh, "ping", args) < 0 ? 1 : 0;
}

static int cmd_ipconfig(Shell *sh, Parsed *p)
{
    if (p->argc > 2 || (p->argc == 2 && _stricmp(p->argv[1], "/all") != 0)) {
        return usage(sh, "ipconfig");
    }
    return run_external(sh, "ipconfig", p->argc == 2 ? "/all" : "") < 0 ? 1 : 0;
}

static int cmd_netinfo(Shell *sh, Parsed *p)
{
    char host[MAX_COMPUTERNAME_LENGTH + 2], dns[256];
    DWORD hn = (DWORD)sizeof host, dn = (DWORD)sizeof dns;
    (void)p;
    print_accent(sh, "Network information (reported by Windows)\n");
    if (GetComputerNameA(host, &hn)) {
        printf("Computer name: %s\n", host);
    }
    if (GetComputerNameExA(ComputerNameDnsDomain, dns, &dn) && dns[0]) {
        printf("DNS domain:    %s\n", dns);
    } else {
        printf("DNS domain:    (none)\n");
    }
    printf("\nAdapter summary from ipconfig.exe:\n");
    return run_external(sh, "ipconfig", "") < 0 ? 1 : 0;
}

static int cmd_sleep(Shell *sh, Parsed *p)
{
    long s;
    if (p->argc != 2 || !parse_long(p->argv[1], &s, 0, 3600)) {
        msg_error(sh, "Seconds must be a whole number from 0 to 3600.");
        printf("Usage: sleep <seconds>\n");
        return 1;
    }
    Sleep((DWORD)s * 1000UL);
    msg_ok(sh, "Slept %ld second(s).", s);
    return 0;
}

/* ======================================================================
 *  SECTION 12: F. developer tools (detect and launch external programs)
 * ==================================================================== */

static int version_check(Shell *sh, Parsed *p, const char *label, const char *exe, const char *hint)
{
    char full[MAX_PATH];
    if (p->argc > 2 || (p->argc == 2 && _stricmp(p->argv[1], "--version") != 0)) {
        return usage(sh, p->argv[0]);
    }
    if (!find_executable(exe, full, sizeof full)) {
        msg_error(sh, "%s was not found on PATH (looked for %s.exe).", label, exe);
        printf("  %s\n", hint);
        return 1;
    }
    printf("Found: %s\n", full);
    return run_external(sh, exe, "--version") < 0 ? 1 : 0;
}

static int cmd_gcc(Shell *sh, Parsed *p)
{
    return version_check(sh, p, "GCC", "gcc",
        "Dev-C++ bundles MinGW; add its 'bin' folder (e.g. C:\\Program Files (x86)\\Dev-Cpp\\MinGW64\\bin) to PATH.");
}

static int cmd_git(Shell *sh, Parsed *p)
{
    return version_check(sh, p, "Git", "git", "Install Git for Windows from https://git-scm.com and reopen the console.");
}

static int cmd_qemu(Shell *sh, Parsed *p)
{
    return version_check(sh, p, "QEMU", "qemu-system-x86_64",
        "Install QEMU for Windows and add its folder to PATH. Nabula cannot boot in QEMU yet; it is not a kernel.");
}

static int cmd_devcheck(Shell *sh, Parsed *p)
{
    static const char *names[] = { "gcc", "git", "qemu-system-x86_64" };
    char full[MAX_PATH];
    int i;
    (void)p;
    for (i = 0; i < 3; i++) {
        if (find_executable(names[i], full, sizeof full)) {
            print_fg(sh, COL_GREEN, "[found]   ");
            printf("%-20s %s\n", names[i], full);
        } else {
            print_fg(sh, COL_YELLOW, "[missing] ");
            printf("%s\n", names[i]);
        }
    }
    return 0;
}

static int program_denied(const char *name)
{
    static const char *deny[] = { "cmd", "command", "powershell", "pwsh", "wscript",
                                  "cscript", "mshta", "wsl", "bash", "sh", NULL };
    const char *b = path_basename(name);
    char stem[64];
    size_t i = 0;
    int k;
    while (b[i] && b[i] != '.' && i < sizeof stem - 1) {
        stem[i] = (char)tolower((unsigned char)b[i]);
        i++;
    }
    stem[i] = '\0';
    for (k = 0; deny[k]; k++) {
        if (strcmp(stem, deny[k]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int cmd_run(Shell *sh, Parsed *p)
{
    char full[MAX_PATH], cmd[4096], q[300];
    const char *prog, *ext;
    size_t len;
    int i, rc;

    if (p->argc < 2) {
        return usage(sh, "run");
    }
    prog = p->argv[1];
    if (!path_ok(sh, prog)) {
        return 1;
    }
    if (program_denied(prog)) {
        msg_error(sh, "Shells and script hosts (cmd, powershell, wscript, wsl...) are blocked by 'run'.");
        return 1;
    }
    if (strpbrk(prog, "\\/:") != NULL) {
        if (path_type(prog) != 1 || !full_path(prog, full, sizeof full)) {
            msg_error(sh, "Program '%s' was not found.", prog);
            return 1;
        }
    } else if (!find_executable(prog, full, sizeof full)) {
        msg_error(sh, "Program '%s' was not found on PATH.", prog);
        return 1;
    }
    ext = strrchr(path_basename(full), '.');
    if (ext == NULL || _stricmp(ext, ".exe") != 0) {
        msg_error(sh, "Only .exe programs can be run (no .bat, .cmd, .vbs, ...).");
        return 1;
    }
    cmd[0] = '"';
    len = 1;
    if (strlen(full) + 4 >= sizeof cmd) {
        msg_error(sh, "Path too long.");
        return 1;
    }
    memcpy(cmd + len, full, strlen(full));
    len += strlen(full);
    cmd[len++] = '"';
    cmd[len] = '\0';
    for (i = 2; i < p->argc; i++) {
        if (!append_quoted(cmd, sizeof cmd, &len, p->argv[i])) {
            msg_error(sh, "Arguments are too long.");
            return 1;
        }
    }
    snprintf(q, sizeof q, "Run '%s' with %d argument(s)?", full, p->argc - 2);
    if (!ask_yes_no(sh, q)) {
        printf("Cancelled.\n");
        return 0;
    }
    rc = run_process(sh, cmd);
    if (rc >= 0) {
        printf("[exit code %d]\n", rc);
    }
    return rc < 0 ? 1 : 0;
}

/* ======================================================================
 *  SECTION 13: G. session and configuration commands
 * ==================================================================== */

static int cmd_config(Shell *sh, Parsed *p)
{
    int i;
    if (p->argc == 1 || (p->argc == 2 && _stricmp(p->argv[1], "show") == 0)) {
        print_accent(sh, "Nabula configuration\n");
        printf("Theme:        %s%s\n", sh->themeName, sh->colorOverride ? " (+ custom text color, not saved)" : "");
        printf("Prompt:       %s\n", sh->prompt);
        printf("Aliases:      %d\n", sh->aliasCount);
        for (i = 0; i < sh->aliasCount; i++) {
            printf("  %s -> %s\n", sh->aliases[i].name, sh->aliases[i].command);
        }
        printf("Config file:  %s\n", sh->configPath);
        printf("History file: %s\n", sh->historyPath);
        return 0;
    }
    if (p->argc == 2 && _stricmp(p->argv[1], "reset") == 0) {
        if (!ask_yes_no(sh, "Reset theme, prompt and ALL aliases to defaults?")) {
            printf("Cancelled.\n");
            return 0;
        }
        set_defaults(sh);
        clear_screen(sh);
        msg_ok(sh, "Defaults restored in memory. Run 'save' to overwrite the saved config file.");
        return 0;
    }
    return usage(sh, "config");
}

static int cmd_alias(Shell *sh, Parsed *p)
{
    char err[160], cmdtext[CMD_MAX];
    if (p->argc < 3) {
        return usage(sh, "alias");
    }
    if (text_from(p, 2, cmdtext, sizeof cmdtext) < 0) {
        return 1;
    }
    if (alias_set(sh, p->argv[1], cmdtext, err, sizeof err) != 0) {
        msg_error(sh, "%s.", err);
        return 1;
    }
    msg_ok(sh, "Alias set: %s -> %s", p->argv[1], cmdtext);
    return 0;
}

static int cmd_unalias(Shell *sh, Parsed *p)
{
    int i;
    if (p->argc != 2) {
        return usage(sh, "unalias");
    }
    for (i = 0; i < sh->aliasCount; i++) {
        if (_stricmp(sh->aliases[i].name, p->argv[1]) == 0) {
            for (; i + 1 < sh->aliasCount; i++) {
                sh->aliases[i] = sh->aliases[i + 1];
            }
            sh->aliasCount--;
            msg_ok(sh, "Alias '%s' removed.", p->argv[1]);
            return 0;
        }
    }
    msg_error(sh, "No alias named '%s'.", p->argv[1]);
    return 1;
}

static int cmd_aliases(Shell *sh, Parsed *p)
{
    int i;
    (void)p;
    if (sh->aliasCount == 0) {
        printf("No aliases defined. Example: alias ll dir\n");
        return 0;
    }
    for (i = 0; i < sh->aliasCount; i++) {
        printf("%-16s -> %s\n", sh->aliases[i].name, sh->aliases[i].command);
    }
    return 0;
}

static int cmd_clearhistory(Shell *sh, Parsed *p)
{
    (void)p;
    sh->histCount = 0;
    msg_ok(sh, "History cleared (in memory). Run 'history save' to overwrite the saved file.");
    return 0;
}

static int cmd_save(Shell *sh, Parsed *p)
{
    int bad = 0;
    (void)p;
    if (write_config(sh) != 0) {
        msg_error(sh, "Could not write %s (%s).", sh->configPath, strerror(errno));
        bad = 1;
    } else {
        msg_ok(sh, "Config saved: %s", sh->configPath);
    }
    if (history_save(sh) != 0) {
        msg_error(sh, "Could not write %s (%s).", sh->historyPath, strerror(errno));
        bad = 1;
    } else {
        msg_ok(sh, "History saved: %s (%d entries)", sh->historyPath, sh->histCount);
    }
    return bad;
}

static int cmd_load(Shell *sh, Parsed *p)
{
    int r, n, bad = 0;
    (void)p;
    r = load_config(sh, 1);
    if (r >= 0) {
        msg_ok(sh, "Config loaded (%d line(s) rejected).", r);
        apply_theme(sh, sh->themeName, 1);
    } else {
        bad = 1;
    }
    n = history_load(sh);
    if (n >= 0) {
        msg_ok(sh, "History loaded: %d entries added.", n);
    } else {
        msg_warn(sh, "No history file found at %s", sh->historyPath);
        bad = 1;
    }
    return bad;
}

/* ======================================================================
 *  SECTION 14: command table and dispatcher
 * ==================================================================== */

static const Command g_commands[] = {
    /* ---- A. core and help ---- */
    { "help", cmd_help, CAT_CORE, "help [command|section]", "List commands or show help for one", BK_NAB, "Sections: core, terminal, files, text, system, dev, session." },
    { "info", cmd_info, CAT_CORE, "info", "Describe this Nabula build and what is real vs hosted", BK_NAB, "" },
    { "version", cmd_version, CAT_CORE, "version", "Show the Nabula version", BK_NAB, "" },
    { "about", cmd_about, CAT_CORE, "about", "About Nabula OS", BK_NAB, "" },
    { "clear", cmd_clear, CAT_CORE, "clear", "Clear the screen", BK_WIN, "Uses the Windows console API." },
    { "cls", cmd_clear, CAT_CORE, "cls", "Clear the screen (same as clear)", BK_WIN, "" },
    { "exit", cmd_exit, CAT_CORE, "exit", "Save history/config and leave Nabula", BK_NAB, "Saves config and history to the data folder on exit." },
    { "date", cmd_date, CAT_CORE, "date", "Show today's date", BK_CRT, "Read-only; does not change the system clock." },
    { "time", cmd_time, CAT_CORE, "time", "Show the current local time", BK_CRT, "Read-only; does not change the system clock." },
    { "uptime", cmd_uptime, CAT_CORE, "uptime", "Show Nabula session and Windows uptime", BK_WIN, "Windows uptime comes from the host." },
    { "whoami", cmd_whoami, CAT_CORE, "whoami", "Show the current Windows user", BK_WIN, "" },
    { "hostname", cmd_hostname, CAT_CORE, "hostname", "Show the computer name", BK_WIN, "" },
    { "status", cmd_status, CAT_CORE, "status", "Show a summary of the current session", BK_NAB, "" },
    { "neofetch", cmd_neofetch, CAT_CORE, "neofetch", "Show a system summary with a logo", BK_WIN, "Hardware and OS values come from Windows." },
    { "history", cmd_history, CAT_CORE, "history [clear|save|load]", "Show or manage command history", BK_NAB, "'save' overwrites history.txt. 'load' appends saved entries. Entries are truncated to 511 characters." },

    /* ---- B. terminal ---- */
    { "theme", cmd_theme, CAT_TERM, "theme [default|dark|light|matrix|ocean|amber]", "Change the color theme", BK_WIN, "Clears the screen to apply the background. Saved by 'save'." },
    { "color", cmd_color, CAT_TERM, "color [name]", "Change the text color", BK_WIN, "Session only. Refuses colors equal to the background." },
    { "prompt", cmd_prompt, CAT_TERM, "prompt [text|reset]", "Show, set or reset the prompt", BK_NAB, "Tokens: {cwd}, {time}. Max 63 characters." },
    { "echo", cmd_echo, CAT_TERM, "echo <text>", "Print text", BK_NAB, "Surrounding double quotes are removed; no variable expansion." },
    { "repeat", cmd_repeat, CAT_TERM, "repeat <count> <text>", "Print text several times", BK_NAB, "Count is limited to 1-100." },
    { "banner", cmd_banner, CAT_TERM, "banner", "Show the Nabula banner", BK_NAB, "" },
    { "beep", cmd_beep, CAT_TERM, "beep", "Play a short beep", BK_WIN, "Depends on the host sound setup." },
    { "pause", cmd_pause, CAT_TERM, "pause", "Wait for Enter", BK_NAB, "" },
    { "confirm", cmd_confirm, CAT_TERM, "confirm [question]", "Ask a yes/no question", BK_NAB, "Only 'y' or 'yes' counts as yes." },

    /* ---- C. filesystem ---- */
    { "pwd", cmd_pwd, CAT_FS, "pwd", "Print the working folder", BK_WIN, "" },
    { "ls", cmd_ls, CAT_FS, "ls [path]", "List a folder (compact)", BK_WIN, "Read-only. No wildcards." },
    { "dir", cmd_dir, CAT_FS, "dir [path]", "List a folder with dates and sizes", BK_WIN, "Read-only. No wildcards." },
    { "cd", cmd_cd, CAT_FS, "cd [path]", "Change the working folder", BK_WIN, "Quote paths with spaces. Without arguments prints the folder." },
    { "mkdir", cmd_mkdir, CAT_FS, "mkdir <name>", "Create one folder", BK_WIN, "Not recursive; parent must exist. Fails if the name exists." },
    { "rmdir", cmd_rmdir, CAT_FS, "rmdir <name>", "Remove an EMPTY folder", BK_WIN, "DESTRUCTIVE: asks for confirmation. Never recursive. Refuses drive roots." },
    { "touch", cmd_touch, CAT_FS, "touch <file>", "Create an empty file or update its time", BK_WIN, "Never truncates existing content." },
    { "cat", cmd_cat, CAT_FS, "cat <file>", "Print a text file", BK_CRT, "Refuses binary-looking files." },
    { "write", cmd_write, CAT_FS, "write <file>", "Type text into a file (replaces contents)", BK_CRT, "DESTRUCTIVE: asks before overwriting. End input with a line containing only '.'." },
    { "append", cmd_append, CAT_FS, "append <file>", "Type text onto the end of a file", BK_CRT, "Creates the file if missing. End input with a line containing only '.'." },
    { "copy", cmd_copy, CAT_FS, "copy <source> <destination>", "Copy a file", BK_WIN, "Files only. Asks before overwriting. A folder destination receives the same file name." },
    { "move", cmd_move, CAT_FS, "move <source> <destination>", "Move a file or folder", BK_WIN, "Asks before replacing a file. Never replaces folders." },
    { "rename", cmd_rename, CAT_FS, "rename <old> <new-name>", "Rename a file or folder in place", BK_WIN, "New name must not contain a path. Never overwrites." },
    { "delete", cmd_delete, CAT_FS, "delete <file>", "Permanently delete one file", BK_WIN, "DESTRUCTIVE: asks for confirmation; bypasses the Recycle Bin; never deletes folders or uses wildcards." },
    { "exists", cmd_exists, CAT_FS, "exists <path>", "Check whether a path exists", BK_WIN, "" },
    { "filesize", cmd_filesize, CAT_FS, "filesize <file>", "Show a file's size", BK_WIN, "" },
    { "tree", cmd_tree, CAT_FS, "tree [path]", "Show a folder tree", BK_WIN, "Depth limit 8, 2000 entries; links are not followed." },
    { "find", cmd_find, CAT_FS, "find <directory> <filename>", "Search for files by name", BK_WIN, "Case-insensitive; * and ? allowed in the name. Max 1000 results, depth 32." },
    { "stat", cmd_stat, CAT_FS, "stat <path>", "Show details about a path", BK_WIN, "" },
    { "type", cmd_cat, CAT_FS, "type <file>", "Print a text file (same as cat)", BK_CRT, "" },
    { "head", cmd_head, CAT_FS, "head <file> [lines]", "Show the first lines (default 10)", BK_CRT, "" },
    { "tail", cmd_tail, CAT_FS, "tail <file> [lines]", "Show the last lines (default 10)", BK_CRT, "Reads the file twice; fine for normal sizes." },

    /* ---- D. text and utilities ---- */
    { "calc", cmd_calc, CAT_TEXT, "calc <a> <operator> <b>", "Calculate with + - * / %", BK_NAB, "Division/modulo by zero rejected. % needs integers; result has the sign of the first number." },
    { "strlen", cmd_strlen, CAT_TEXT, "strlen <text>", "Count bytes in text", BK_NAB, "Counts bytes, not Unicode characters." },
    { "reverse", cmd_reverse, CAT_TEXT, "reverse <text>", "Reverse text", BK_NAB, "Byte-wise; non-ASCII text may be garbled." },
    { "upper", cmd_upper, CAT_TEXT, "upper <text>", "Convert text to upper case", BK_NAB, "ASCII letters only." },
    { "lower", cmd_lower, CAT_TEXT, "lower <text>", "Convert text to lower case", BK_NAB, "ASCII letters only." },
    { "count", cmd_count, CAT_TEXT, "count <file>", "Count lines, words and bytes of a file", BK_CRT, "" },
    { "random", cmd_random, CAT_TEXT, "random [min] [max]", "Random whole number", BK_CRT, "No args: 1-100. One arg: 1-max. Not cryptographically secure." },
    { "timestamp", cmd_timestamp, CAT_TEXT, "timestamp", "Show Unix time, UTC and local time", BK_CRT, "" },
    { "env", cmd_env, CAT_TEXT, "env", "List environment variables", BK_WIN, "Read-only. May reveal sensitive values; be careful when sharing output." },
    { "getenv", cmd_getenv, CAT_TEXT, "getenv <name>", "Show one environment variable", BK_WIN, "Read-only." },
    { "which", cmd_which, CAT_TEXT, "which <command>", "Tell where a command comes from", BK_NAB, "Checks aliases, built-ins, then PATH for .exe files." },
    { "man", cmd_man, CAT_TEXT, "man <command>", "Show the manual entry for a command", BK_NAB, "" },

    /* ---- E. process and system ---- */
    { "sysinfo", cmd_sysinfo, CAT_SYS, "sysinfo", "Summarize the host system", BK_WIN, "Values come from Windows." },
    { "meminfo", cmd_meminfo, CAT_SYS, "meminfo", "Show memory usage", BK_WIN, "Reports Windows memory; Nabula has no memory manager." },
    { "cpuinfo", cmd_cpuinfo, CAT_SYS, "cpuinfo", "Show CPU details", BK_WIN, "Model/speed read from the Windows registry." },
    { "diskinfo", cmd_diskinfo, CAT_SYS, "diskinfo", "Show drives and free space", BK_WIN, "Network drives may be slow to respond." },
    { "processes", cmd_processes, CAT_SYS, "processes", "List running Windows processes", BK_WIN, "Uses the ToolHelp API; shows Windows processes, not Nabula tasks." },
    { "tasklist", cmd_tasklist, CAT_SYS, "tasklist", "Run Windows tasklist.exe", BK_EXT, "No arguments accepted. Some details need administrator rights." },
    { "ping", cmd_ping, CAT_SYS, "ping <host> [count]", "Ping a host (1-10 times, default 4)", BK_EXT, "Runs ping.exe. Host must be a plain name or IP; no shell is involved." },
    { "netinfo", cmd_netinfo, CAT_SYS, "netinfo", "Show network names and adapter summary", BK_EXT, "Uses ipconfig.exe." },
    { "ipconfig", cmd_ipconfig, CAT_SYS, "ipconfig [/all]", "Run Windows ipconfig.exe", BK_EXT, "Only the /all switch is accepted." },
    { "sleep", cmd_sleep, CAT_SYS, "sleep <seconds>", "Wait 0-3600 seconds", BK_WIN, "Ctrl+C interrupts and exits Nabula." },

    /* ---- F. developer tools ---- */
    { "gcc", cmd_gcc, CAT_DEV, "gcc [--version]", "Check for GCC and show its version", BK_EXT, "Only --version is supported; Nabula is not a compiler front end." },
    { "git", cmd_git, CAT_DEV, "git [--version]", "Check for Git and show its version", BK_EXT, "Only --version is supported." },
    { "qemu", cmd_qemu, CAT_DEV, "qemu [--version]", "Check for QEMU and show its version", BK_EXT, "Looks for qemu-system-x86_64.exe. Only --version is supported." },
    { "devcheck", cmd_devcheck, CAT_DEV, "devcheck", "Check which developer tools are installed", BK_WIN, "Searches PATH only; does not launch anything." },
    { "run", cmd_run, CAT_DEV, "run <program.exe> [args...]", "Run a Windows program after confirmation", BK_EXT, "Only .exe files; shells and script hosts are blocked; no shell parsing. Asks before every run. Not a security sandbox: the program has your full permissions." },

    /* ---- G. session ---- */
    { "config", cmd_config, CAT_SESSION, "config [show|reset]", "Show or reset settings", BK_NAB, "'reset' asks for confirmation and affects memory until you 'save'." },
    { "alias", cmd_alias, CAT_SESSION, "alias <name> <command>", "Create a shortcut for a Nabula command", BK_NAB, "Target must start with a built-in Nabula command; aliases cannot run shell text or other aliases. Max 31 aliases." },
    { "unalias", cmd_unalias, CAT_SESSION, "unalias <name>", "Remove an alias", BK_NAB, "" },
    { "aliases", cmd_aliases, CAT_SESSION, "aliases", "List aliases", BK_NAB, "" },
    { "clearhistory", cmd_clearhistory, CAT_SESSION, "clearhistory", "Clear command history", BK_NAB, "In memory only until 'history save' or 'exit'." },
    { "save", cmd_save, CAT_SESSION, "save", "Save config and history to disk", BK_CRT, "Overwrites config.ini and history.txt in the data folder." },
    { "load", cmd_load, CAT_SESSION, "load", "Load config and history from disk", BK_CRT, "Config is validated line by line and never executed. Aliases merge; history entries are appended." }
};

static const Command *command_table(int *count)
{
    *count = (int)(sizeof g_commands / sizeof g_commands[0]);
    return g_commands;
}

static const Command *find_command(const char *name)
{
    int i, n = (int)(sizeof g_commands / sizeof g_commands[0]);
    for (i = 0; i < n; i++) {
        if (_stricmp(g_commands[i].name, name) == 0) {
            return &g_commands[i];
        }
    }
    return NULL;
}

static int dispatch(Shell *sh, const char *line)
{
    Parsed p, p2, *use = &p;
    char err[160], expanded[CMD_MAX + ALIAS_CMD_MAX + 4], name[64];
    const Command *c;
    Alias *a;
    const char *rest;

    if (parse_line(line, &p, err, sizeof err) != 0) {
        msg_error(sh, "%s", err);
        return 1;
    }
    if (p.argc == 0) {
        return 0;
    }
    if (strlen(p.argv[0]) >= sizeof name) {
        msg_error(sh, "Command name too long.");
        return 1;
    }
    snprintf(name, sizeof name, "%s", p.argv[0]);
    str_lower(name);

    /* One level of alias expansion; aliases always point to built-ins. */
    a = find_alias(sh, name);
    if (a != NULL) {
        rest = (p.argc > 1) ? p.raw + p.off[1] : "";
        if (snprintf(expanded, sizeof expanded, "%s%s%s", a->command, *rest ? " " : "", rest)
            >= CMD_MAX) {
            msg_error(sh, "Expanded alias command is too long.");
            return 1;
        }
        if (parse_line(expanded, &p2, err, sizeof err) != 0) {
            msg_error(sh, "%s", err);
            return 1;
        }
        use = &p2;
        snprintf(name, sizeof name, "%s", use->argv[0]);
        str_lower(name);
    }
    c = find_command(name);
    if (c == NULL) {
        msg_error(sh, "Unknown command '%s'. Type 'help' for the list.", use->argv[0]);
        return 1;
    }
    return c->fn(sh, use);
}

/* ======================================================================
 *  SECTION 15: startup, shutdown, main loop
 * ==================================================================== */

static void init_shell(Shell *sh)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    memset(sh, 0, sizeof *sh);
    sh->hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    sh->origAttr = 0x07;
    if (GetConsoleScreenBufferInfo(sh->hOut, &csbi)) {
        sh->origAttr = csbi.wAttributes;
    }
    g_hOut = sh->hOut;
    g_origAttr = sh->origAttr;
    sh->attr = sh->origAttr;
    sh->running = 1;
    sh->startTime = time(NULL);

    SetErrorMode(SEM_FAILCRITICALERRORS);   /* no "insert disk" pop-ups in diskinfo */
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    SetConsoleTitleA("Nabula OS v" NABULA_VERSION);
    srand((unsigned)time(NULL));

    set_defaults(sh);
    init_paths(sh);
}

static void shutdown_shell(Shell *sh)
{
    if (write_config(sh) != 0) {
        msg_warn(sh, "Could not save config to %s", sh->configPath);
    }
    if (history_save(sh) != 0) {
        msg_warn(sh, "Could not save history to %s", sh->historyPath);
    }
    printf("Session saved to %s\nGoodbye from Nabula OS.\n", sh->dataDir);
    restore_console_raw(sh->hOut, sh->origAttr);
}

int main(void)
{
    static Shell sh;
    char line[CMD_MAX + 2];
    char *cmd;
    int tooLong;

    init_shell(&sh);
    load_config(&sh, 0);
    history_load(&sh);
    clear_screen(&sh);
    print_banner(&sh);
    printf("Type 'help' to list commands. This is a hosted CLI, not a kernel.\n\n");

    while (sh.running) {
        print_prompt(&sh);
        if (!read_line(line, sizeof line, &tooLong)) {
            printf("\n");
            break;  /* end of input */
        }
        if (tooLong || strlen(line) >= CMD_MAX) {
            msg_error(&sh, "Command too long (max %d characters).", CMD_MAX - 1);
            continue;
        }
        cmd = trim(line);
        if (*cmd == '\0') {
            continue;
        }
        if (has_control_chars(cmd)) {
            msg_error(&sh, "Input contains control characters and was rejected.");
            continue;
        }
        history_add(&sh, cmd);
        dispatch(&sh, cmd);
    }
    shutdown_shell(&sh);
    return 0;
}