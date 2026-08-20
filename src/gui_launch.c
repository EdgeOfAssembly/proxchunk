/**
 * @file gui_launch.c
 * @brief Fully static musl trampoline for the bundled GTK GUI.
 *
 * On a glibc host the musl GUI binary's PT_INTERP (`/lib/ld-musl-…`) does
 * not exist. This launcher execs the bundled loader with `--library-path`
 * pointing at `$ORIGIN/lib`. If no bundled loader is present, exec the
 * GTK binary directly (fully static GUI).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
die(const char* msg)
{
    fprintf(stderr, "proxchunk-gui: %s: %s\n", msg, strerror(errno));
    _exit(1);
}

static void
set_if_exists(const char* key, const char* path)
{
    if (access(path, R_OK) == 0)
    {
        setenv(key, path, 1);
    }
}

int
main(int argc, char** argv)
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
    {
        die("readlink /proc/self/exe");
    }
    exe[n] = '\0';
    char* slash = strrchr(exe, '/');
    if (slash == NULL)
    {
        die("binary path");
    }
    *slash = '\0';
    const char* root = exe;

    char rootenv[4224];
    if (snprintf(rootenv, sizeof(rootenv), "PROXCHUNK_ROOT=%s", root) >= (int)sizeof(rootenv))
    {
        die("PROXCHUNK_ROOT too long");
    }
    char* rootcopy = strdup(rootenv);
    if (rootcopy == NULL)
    {
        die("strdup");
    }
    putenv(rootcopy);

    char lib[4224];
    char loader[4224];
    char bin[4224];
    char pixbuf[4224];
    char pixdir[4224];
    char gio[4224];
    char share[4224];
    snprintf(lib, sizeof(lib), "%s/lib", root);
    snprintf(loader, sizeof(loader), "%s/lib/ld-musl-x86_64.so.1", root);
    snprintf(bin, sizeof(bin), "%s/libexec/proxchunk-gui.bin", root);
    if (access(bin, X_OK) != 0)
    {
        snprintf(bin, sizeof(bin), "%s/proxchunk-gui.bin", root);
    }

    snprintf(pixdir, sizeof(pixdir), "%s/lib/gdk-pixbuf-2.0/2.10.0/loaders", root);
    set_if_exists("GDK_PIXBUF_MODULEDIR", pixdir);
    {
        char loader[4224];
        char cache[4224];
        snprintf(loader, sizeof(loader),
                 "%s/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-xpm.so", root);
        snprintf(cache, sizeof(cache),
                 "%s/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache", root);
        if (access(loader, R_OK) == 0)
        {
            FILE* f = fopen(cache, "w");
            if (f == NULL)
            {
                const char* home = getenv("HOME");
                if (home != NULL)
                {
                    char dir[4224];
                    snprintf(dir, sizeof(dir), "%s/.cache/proxchunk", home);
                    mkdir(dir, 0755);
                    snprintf(cache, sizeof(cache), "%s/.cache/proxchunk/loaders.cache", home);
                    f = fopen(cache, "w");
                }
            }
            if (f != NULL)
            {
                fprintf(f, "# GdkPixbuf Image Loader Modules file\n");
                fprintf(f, "\"%s\"\n", loader);
                fprintf(f, "\"legacy-xpm\" 4 \"gdk-pixbuf\" \"XPM\" \"LGPL\"\n");
                fprintf(f, "\"image/x-xpixmap\" \"\"\n\"\"\n");
                fprintf(f, "\"/* XPM */\" \"\" 100\n");
                fclose(f);
                setenv("GDK_PIXBUF_MODULE_FILE", cache, 1);
            }
        }
    }
    setenv("GTK_IM_MODULE", "gtk-im-context-simple", 1);
    snprintf(gio, sizeof(gio), "%s/lib/gio/modules", root);
    if (access(gio, X_OK) == 0)
    {
        setenv("GIO_MODULE_DIR", gio, 1);
    }
    snprintf(share, sizeof(share), "%s/share", root);
    const char* old_xdg = getenv("XDG_DATA_DIRS");
    char xdg[8192];
    snprintf(xdg, sizeof(xdg), "%s%s%s", share, (old_xdg != NULL) ? ":" : "",
             (old_xdg != NULL) ? old_xdg : "");
    setenv("XDG_DATA_DIRS", xdg, 1);
    setenv("GDK_BACKEND", "x11", 1);
    setenv("GTK_CSD", "0", 1);

    if (access(loader, X_OK) == 0)
    {
        char** nargv = calloc((size_t)argc + 5U, sizeof(char*));
        if (nargv == NULL)
        {
            die("calloc");
        }
        nargv[0] = loader;
        nargv[1] = "--library-path";
        nargv[2] = lib;
        nargv[3] = bin;
        for (int i = 1; i < argc; ++i)
        {
            nargv[3 + i] = argv[i];
        }
        execv(loader, nargv);
        die("exec bundled ld-musl");
    }

    argv[0] = bin;
    execv(bin, argv);
    die("exec proxchunk-gui.bin");
}
