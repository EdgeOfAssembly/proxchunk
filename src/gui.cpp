/*
 * proxchunk-gui — thin GTK+3 + VTE wrapper around `proxchunk --repl`.
 */

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <vte/vte.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef PROXCHUNK_VERSION
#define PROXCHUNK_VERSION "1.1"
#endif

namespace fs = std::filesystem;

namespace {

std::string
exe_path()
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
    {
        return {};
    }
    buf[static_cast<std::size_t>(n)] = '\0';
    return std::string(buf);
}

std::string
install_root()
{
    if (const char* r = std::getenv("PROXCHUNK_ROOT"); r != nullptr && r[0] != '\0')
    {
        return std::string(r);
    }
    fs::path p(exe_path());
    if (p.empty())
    {
        return ".";
    }
    p = p.parent_path();
    if (p.filename() == "libexec")
    {
        p = p.parent_path();
    }
    return p.string();
}

std::string
find_cli(const std::string& root)
{
    const fs::path a = fs::path(root) / "proxchunk";
    if (fs::is_regular_file(a))
    {
        return a.string();
    }
    const fs::path b = fs::path(exe_path()).parent_path() / "proxchunk";
    if (fs::is_regular_file(b))
    {
        return b.string();
    }
    return "proxchunk";
}

std::string
find_icon(const std::string& root)
{
    const char* rel[] = {
        "icons/hicolor/48x48/apps/proxchunk.png",
        "icons/pixmaps/proxchunk.png",
        "icons/proxchunk.png",
        nullptr,
    };
    for (int i = 0; rel[i] != nullptr; ++i)
    {
        fs::path p = fs::path(root) / rel[i];
        if (fs::is_regular_file(p))
        {
            return p.string();
        }
    }
    return {};
}

struct Gui
{
    GtkWidget* window = nullptr;
    VteTerminal* vte = nullptr;
    std::string root;
    std::string cli;
    std::string icon;
    std::string cli_storage;
};

void
on_quit(GtkWidget* /*w*/, gpointer /*data*/)
{
    gtk_main_quit();
}

void
on_about(GtkWidget* /*w*/, gpointer data)
{
    auto* g = static_cast<Gui*>(data);
    GtkWidget* dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dlg), "About proxchunk");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(g->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 16);
    gtk_container_add(GTK_CONTAINER(dlg), vbox);

    if (!g->icon.empty())
    {
        GtkWidget* img = gtk_image_new_from_file(g->icon.c_str());
        gtk_box_pack_start(GTK_BOX(vbox), img, FALSE, FALSE, 0);
    }
    GtkWidget* name = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(name), "<b>proxchunk</b>");
    gtk_box_pack_start(GTK_BOX(vbox), name, FALSE, FALSE, 0);

    GtkWidget* ver = gtk_label_new("proxchunk " PROXCHUNK_VERSION);
    gtk_box_pack_start(GTK_BOX(vbox), ver, FALSE, FALSE, 0);

    GtkWidget* copy = gtk_label_new("Copyright (c) 2026 proxchunk contributors");
    gtk_box_pack_start(GTK_BOX(vbox), copy, FALSE, FALSE, 0);

    GtkWidget* ok = gtk_button_new_with_label("OK");
    gtk_box_pack_start(GTK_BOX(vbox), ok, FALSE, FALSE, 0);
    g_signal_connect_swapped(ok, "clicked", G_CALLBACK(gtk_widget_destroy), dlg);

    gtk_widget_show_all(dlg);
}

gboolean
on_key(GtkWidget* /*w*/, GdkEventKey* ev, gpointer data)
{
    auto* g = static_cast<Gui*>(data);
    const guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (mods == GDK_CONTROL_MASK
        && (ev->keyval == GDK_KEY_v || ev->keyval == GDK_KEY_V))
    {
        vte_terminal_paste_clipboard(g->vte);
        return TRUE;
    }
    if (mods == GDK_SHIFT_MASK && ev->keyval == GDK_KEY_Insert)
    {
        vte_terminal_paste_clipboard(g->vte);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK
        && (ev->keyval == GDK_KEY_q || ev->keyval == GDK_KEY_Q))
    {
        gtk_main_quit();
        return TRUE;
    }
    return FALSE;
}

void
on_child_exited(VteTerminal* /*t*/, gint /*status*/, gpointer /*data*/)
{
    gtk_main_quit();
}

void
on_spawn(VteTerminal* /*t*/, GPid pid, GError* error, gpointer data)
{
    auto* g = static_cast<Gui*>(data);
    if (error != nullptr)
    {
        GtkWidget* dlg = gtk_message_dialog_new(
            GTK_WINDOW(g->window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "Could not start proxchunk:\n%s",
            error->message);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        gtk_main_quit();
        return;
    }
    (void)pid;
}

GtkWidget*
make_menubar(Gui* g, GtkAccelGroup* accel)
{
    GtkWidget* bar = gtk_menu_bar_new();

    GtkWidget* file_item = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget* file_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    GtkWidget* quit = gtk_menu_item_new_with_mnemonic("_Quit");
    gtk_widget_add_accelerator(quit, "activate", accel, GDK_KEY_q, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit), g);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file_item);

    GtkWidget* help_item = gtk_menu_item_new_with_mnemonic("_Help");
    GtkWidget* help_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);
    GtkWidget* about = gtk_menu_item_new_with_mnemonic("_About");
    g_signal_connect(about, "activate", G_CALLBACK(on_about), g);
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), about);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), help_item);

    return bar;
}

} // namespace

int
main(int argc, char* argv[])
{
    const std::string root_early = install_root();
    g_setenv("GTK_CSD", "0", TRUE);
    g_setenv("GTK_IM_MODULE", "gtk-im-context-simple", TRUE);
    g_setenv("GDK_BACKEND", "x11", TRUE);
    {
        fs::path gio = fs::path(root_early) / "lib" / "gio" / "modules";
        if (fs::is_directory(gio))
        {
            g_setenv("GIO_MODULE_DIR", gio.string().c_str(), TRUE);
        }
    }
    gtk_init(&argc, &argv);
    if (GtkSettings* st = gtk_settings_get_default(); st != nullptr)
    {
        g_object_set(st,
                     "gtk-button-images", FALSE,
                     "gtk-menu-images", FALSE,
                     "gtk-dialogs-use-header", FALSE,
                     "gtk-decoration-layout", "menu:",
                     "gtk-icon-theme-name", "hicolor",
                     nullptr);
    }

    Gui g;
    g.root = install_root();
    g.cli_storage = find_cli(g.root);
    g.cli = g.cli_storage;
    g.icon = find_icon(g.root);

    g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g.window), "proxchunk " PROXCHUNK_VERSION);
    gtk_window_set_default_size(GTK_WINDOW(g.window), 900, 560);
    if (!g.icon.empty())
    {
        gtk_window_set_icon_from_file(GTK_WINDOW(g.window), g.icon.c_str(), nullptr);
    }
    g_signal_connect(g.window, "destroy", G_CALLBACK(on_quit), nullptr);

    GtkAccelGroup* accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(g.window), accel);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g.window), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), make_menubar(&g, accel), FALSE, FALSE, 0);

    GtkWidget* term = vte_terminal_new();
    g.vte = VTE_TERMINAL(term);
    vte_terminal_set_scrollback_lines(g.vte, 4000);
    vte_terminal_set_mouse_autohide(g.vte, TRUE);
    vte_terminal_set_cursor_blink_mode(g.vte, VTE_CURSOR_BLINK_SYSTEM);
    gtk_widget_set_hexpand(term, TRUE);
    gtk_widget_set_vexpand(term, TRUE);

    gtk_box_pack_start(GTK_BOX(vbox), term, TRUE, TRUE, 0);

    g_signal_connect(g.window, "key-press-event", G_CALLBACK(on_key), &g);
    g_signal_connect(term, "key-press-event", G_CALLBACK(on_key), &g);
    g_signal_connect(term, "child-exited", G_CALLBACK(on_child_exited), &g);

    std::vector<char*> av;
    av.push_back(g.cli_storage.data());
    av.push_back(const_cast<char*>("--repl"));
    av.push_back(nullptr);

    vte_terminal_spawn_async(
        g.vte,
        VTE_PTY_DEFAULT,
        nullptr,
        av.data(),
        nullptr,
        G_SPAWN_DEFAULT,
        nullptr,
        nullptr,
        nullptr,
        -1,
        nullptr,
        on_spawn,
        &g);

    gtk_widget_show_all(g.window);
    gtk_widget_grab_focus(term);
    gtk_main();
    return 0;
}
