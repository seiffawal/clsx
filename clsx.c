/*
 * clsx - "cls" with a safety net for Windows cmd.
 *
 *   cls              save what is on the screen, then clear it
 *   cls --back [N]   print the Nth most recent saved screen (default 1)
 *   cls --list       list saved screens, newest first
 *   cls --install    make `cls` run clsx in every new cmd window
 *   cls --uninstall  undo --install
 *
 * Saved screens live in %LOCALAPPDATA%\clsx as plain UTF-8 text files.
 * The newest 20 are kept; set CLSX_KEEP=50 to keep more.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -O2 -Wall -municode -static -o clsx.exe clsx.c -ladvapi32
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define KEEP_DEFAULT 20
#define MAX_LINES    5000   /* never save more than this many rows */
#define MAX_FILES    4096

typedef struct { wchar_t name[48]; } Entry;

static wchar_t g_dir[MAX_PATH];
static Entry   g_entries[MAX_FILES];

/* ---------- output helpers ---------- */

static char *to_utf8(const wchar_t *w, size_t wlen, size_t *outlen) {
    int need = WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, NULL, 0, NULL, NULL);
    char *s;
    if (need < 0) need = 0;
    s = (char *)malloc((size_t)need + 1);
    if (!s) return NULL;
    if (need > 0)
        WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, s, need, NULL, NULL);
    s[need] = 0;
    *outlen = (size_t)need;
    return s;
}

static wchar_t *from_utf8(const char *s, size_t len, size_t *outlen) {
    int need = len ? MultiByteToWideChar(CP_UTF8, 0, s, (int)len, NULL, 0) : 0;
    wchar_t *w;
    if (need < 0) need = 0;
    w = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    if (need > 0)
        MultiByteToWideChar(CP_UTF8, 0, s, (int)len, w, need);
    w[need] = 0;
    *outlen = (size_t)need;
    return w;
}

static void out_text(const wchar_t *w, size_t len) {
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(o, &mode)) {
        size_t i = 0;
        while (i < len) {
            DWORD chunk = (DWORD)((len - i) > 8192 ? 8192 : (len - i));
            DWORD wr;
            WriteConsoleW(o, w + i, chunk, &wr, NULL);
            i += chunk;
        }
    } else {
        size_t ul;
        char *u = to_utf8(w, len, &ul);
        if (u) {
            DWORD wr;
            WriteFile(o, u, (DWORD)ul, &wr, NULL);
            free(u);
        }
    }
}

static void say(const wchar_t *fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    out_text(buf, wcslen(buf));
}

/* ---------- storage ---------- */

static int init_dir(void) {
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", g_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return 0;
    wcscat(g_dir, L"\\clsx");
    CreateDirectoryW(g_dir, NULL); /* fine if it already exists */
    return 1;
}

static int keep_count(void) {
    wchar_t v[16];
    DWORD n = GetEnvironmentVariableW(L"CLSX_KEEP", v, 16);
    if (n > 0 && n < 16) {
        int k = _wtoi(v);
        if (k >= 1 && k <= 1000) return k;
    }
    return KEEP_DEFAULT;
}

static void full_path(wchar_t *dst, const wchar_t *name) {
    _snwprintf(dst, MAX_PATH + 63, L"%ls\\%ls", g_dir, name);
    dst[MAX_PATH + 63] = 0;
}

static int cmp_entry(const void *a, const void *b) {
    return wcscmp(((const Entry *)a)->name, ((const Entry *)b)->name);
}

/* fills g_entries sorted oldest -> newest, returns the count */
static int list_files(void) {
    wchar_t pat[MAX_PATH + 8];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    _snwprintf(pat, MAX_PATH + 7, L"%ls\\*.txt", g_dir);
    pat[MAX_PATH + 7] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (n < MAX_FILES && wcslen(fd.cFileName) < 48)
            wcscpy(g_entries[n++].name, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(g_entries, (size_t)n, sizeof(Entry), cmp_entry);
    return n;
}

static void prune(void) {
    int keep = keep_count();
    int n = list_files();
    int i;
    for (i = 0; i < n - keep; i++) {
        wchar_t p[MAX_PATH + 64];
        full_path(p, g_entries[i].name);
        DeleteFileW(p);
    }
}

static wchar_t *read_text(const wchar_t *path, size_t *outlen) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    DWORD sz, rd = 0;
    char *raw;
    wchar_t *w;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    sz = GetFileSize(f, NULL);
    if (sz == INVALID_FILE_SIZE) { CloseHandle(f); return NULL; }
    raw = (char *)malloc((size_t)sz + 1);
    if (!raw || !ReadFile(f, raw, sz, &rd, NULL)) {
        free(raw);
        CloseHandle(f);
        return NULL;
    }
    CloseHandle(f);
    w = from_utf8(raw, rd, outlen);
    free(raw);
    return w;
}

/* ---------- screen capture ---------- */

/* Reads the console text from the top of the buffer to the cursor row. */
static wchar_t *capture(size_t *outlen) {
    HANDLE h = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    CONSOLE_SCREEN_BUFFER_INFO info;
    wchar_t *buf, *line;
    size_t n = 0;
    int width, first, last, y;

    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetConsoleScreenBufferInfo(h, &info)) { CloseHandle(h); return NULL; }

    width = info.dwSize.X;
    last  = info.dwCursorPosition.Y;
    first = 0;
    if (last - first + 1 > MAX_LINES) first = last - MAX_LINES + 1;

    buf  = (wchar_t *)malloc(((size_t)(last - first + 1) * (size_t)(width + 1) + 1) * sizeof(wchar_t));
    line = (wchar_t *)malloc(((size_t)width + 1) * sizeof(wchar_t));
    if (!buf || !line) { free(buf); free(line); CloseHandle(h); return NULL; }

    for (y = first; y <= last; y++) {
        COORD c;
        DWORD got = 0, k;
        c.X = 0;
        c.Y = (SHORT)y;
        if (!ReadConsoleOutputCharacterW(h, line, (DWORD)width, c, &got)) got = 0;
        for (k = 0; k < got; k++)
            if (line[k] == 0) line[k] = L' ';
        while (got > 0 && line[got - 1] == L' ') got--;
        memcpy(buf + n, line, got * sizeof(wchar_t));
        n += got;
        buf[n++] = L'\n';
    }
    free(line);
    CloseHandle(h);

    /* drop the empty rows at the bottom */
    while (n >= 2 && buf[n - 1] == L'\n' && buf[n - 2] == L'\n') n--;
    buf[n] = 0;
    *outlen = n;
    return buf;
}

static void save_screen(void) {
    size_t n = 0, i;
    wchar_t *text = capture(&n);
    int any = 0;
    SYSTEMTIME st;
    wchar_t name[48], path[MAX_PATH + 64];
    char *u;
    size_t ul;
    HANDLE f;
    DWORD wr;

    if (!text) return;
    for (i = 0; i < n; i++)
        if (text[i] != L'\n' && text[i] != L' ') { any = 1; break; }
    if (!any) { free(text); return; }

    GetLocalTime(&st);
    _snwprintf(name, 47, L"%04d%02d%02d-%02d%02d%02d-%03d.txt",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    name[47] = 0;
    full_path(path, name);

    u = to_utf8(text, n, &ul);
    free(text);
    if (!u) return;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, u, (DWORD)ul, &wr, NULL);
        CloseHandle(f);
    }
    free(u);
    prune();
}

/* Runs the real, built-in cls (/d skips AutoRun so nothing recurses). */
static void run_native_cls(void) {
    wchar_t comspec[MAX_PATH], cmdline[MAX_PATH + 32];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (!GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH))
        wcscpy(comspec, L"cmd.exe");
    _snwprintf(cmdline, MAX_PATH + 31, L"\"%ls\" /d /c cls", comspec);
    cmdline[MAX_PATH + 31] = 0;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* ---------- commands ---------- */

static int cmd_back(int nth) {
    int n = list_files();
    wchar_t path[MAX_PATH + 64];
    size_t len = 0;
    wchar_t *text;
    if (n == 0) { say(L"clsx: no saved screens yet.\n"); return 1; }
    if (nth < 1 || nth > n) {
        say(L"clsx: only %d saved screen(s).\n", n);
        return 1;
    }
    full_path(path, g_entries[n - nth].name);
    text = read_text(path, &len);
    if (!text) { say(L"clsx: could not read the saved screen.\n"); return 1; }
    out_text(text, len);
    free(text);
    return 0;
}

static int cmd_list(void) {
    int n = list_files(), i, idx = 1;
    if (n == 0) { say(L"clsx: no saved screens yet.\n"); return 0; }
    for (i = n - 1; i >= 0; i--, idx++) {
        const wchar_t *nm = g_entries[i].name;
        wchar_t path[MAX_PATH + 64];
        size_t len = 0, k;
        int lines = 0;
        wchar_t *t;
        full_path(path, nm);
        t = read_text(path, &len);
        if (t) {
            for (k = 0; k < len; k++) if (t[k] == L'\n') lines++;
            free(t);
        }
        say(L"%3d  %.4ls-%.2ls-%.2ls %.2ls:%.2ls:%.2ls  %d lines\n",
            idx, nm, nm + 4, nm + 6, nm + 9, nm + 11, nm + 13, lines);
    }
    say(L"\nShow one with:  cls --back N\n");
    return 0;
}

/* Adds/removes a doskey macro in HKCU\...\Command Processor\AutoRun. */
static int autorun(int remove) {
    wchar_t exe[MAX_PATH], frag[MAX_PATH + 32], cur[4096];
    HKEY k;
    DWORD type = REG_SZ, sz = sizeof(cur) - sizeof(wchar_t);
    LONG r;
    wchar_t *pos;
    size_t fl;

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf(frag, MAX_PATH + 31, L"doskey cls=\"%ls\" $*", exe);
    frag[MAX_PATH + 31] = 0;
    fl = wcslen(frag);

    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Command Processor",
                        0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) {
        say(L"clsx: could not open the registry key.\n");
        return 1;
    }
    cur[0] = 0;
    r = RegQueryValueExW(k, L"AutoRun", NULL, &type, (BYTE *)cur, &sz);
    if (r == ERROR_MORE_DATA) {
        say(L"clsx: your existing AutoRun value is too long to edit safely.\n");
        RegCloseKey(k);
        return 1;
    }
    if (r != ERROR_SUCCESS) { cur[0] = 0; type = REG_SZ; }
    else cur[sz / sizeof(wchar_t)] = 0;

    pos = wcsstr(cur, frag);
    if (!remove) {
        if (pos) {
            say(L"clsx: already installed.\n");
        } else {
            wchar_t out[4096 + MAX_PATH + 48];
            if (cur[0]) _snwprintf(out, sizeof(out) / sizeof(wchar_t) - 1, L"%ls & %ls", cur, frag);
            else        _snwprintf(out, sizeof(out) / sizeof(wchar_t) - 1, L"%ls", frag);
            out[sizeof(out) / sizeof(wchar_t) - 1] = 0;
            RegSetValueExW(k, L"AutoRun", 0, type, (BYTE *)out,
                           (DWORD)((wcslen(out) + 1) * sizeof(wchar_t)));
            say(L"clsx: installed. Open a NEW cmd window; `cls` now saves before clearing.\n");
        }
    } else {
        if (!pos) {
            say(L"clsx: not installed.\n");
        } else {
            if (pos - cur >= 3 && wcsncmp(pos - 3, L" & ", 3) == 0)
                memmove(pos - 3, pos + fl, (wcslen(pos + fl) + 1) * sizeof(wchar_t));
            else if (wcsncmp(pos + fl, L" & ", 3) == 0)
                memmove(pos, pos + fl + 3, (wcslen(pos + fl + 3) + 1) * sizeof(wchar_t));
            else
                memmove(pos, pos + fl, (wcslen(pos + fl) + 1) * sizeof(wchar_t));
            if (cur[0] == 0) RegDeleteValueW(k, L"AutoRun");
            else RegSetValueExW(k, L"AutoRun", 0, type, (BYTE *)cur,
                                (DWORD)((wcslen(cur) + 1) * sizeof(wchar_t)));
            say(L"clsx: removed. New cmd windows use the normal cls again.\n");
        }
    }
    RegCloseKey(k);
    return 0;
}

static void help(void) {
    say(L"clsx - cls with a safety net\n\n"
        L"  cls              save the screen, then clear it\n"
        L"  cls --back [N]   show the Nth most recent saved screen (default 1)\n"
        L"  cls --list       list saved screens\n"
        L"  cls --install    make cls use clsx in every new cmd window\n"
        L"  cls --uninstall  undo --install\n\n"
        L"Saved in %%LOCALAPPDATA%%\\clsx (newest 20 kept; set CLSX_KEEP to change).\n");
}

int wmain(int argc, wchar_t **argv) {
    int have_dir = init_dir();

    if (argc == 1) {
        if (have_dir) save_screen();
        run_native_cls();
        return 0;
    }
    if (!_wcsicmp(argv[1], L"--install"))   return autorun(0);
    if (!_wcsicmp(argv[1], L"--uninstall")) return autorun(1);
    if (!have_dir) { say(L"clsx: %%LOCALAPPDATA%% is not set.\n"); return 1; }
    if (!_wcsicmp(argv[1], L"--list"))      return cmd_list();
    if (!_wcsicmp(argv[1], L"--back") || !_wcsicmp(argv[1], L"-b"))
        return cmd_back(argc > 2 ? _wtoi(argv[2]) : 1);
    help();
    return 0;
}
