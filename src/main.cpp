// Proton Drive GUI - a GTK4 desktop client for Proton Drive
// Author: Jean-Francois Lachance-Caumartin
#include <gtk/gtk.h>

#include "proton_client.hpp"

#include <thread>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <cstdio>

#define APP_ID "io.github.jflc.ProtonDriveGUI"
#define APP_VERSION "1.0"

namespace {

struct PathEntry { std::string linkId; std::string name; };

struct App {
    GtkApplication* app = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* stack = nullptr;

    // Login view
    GtkWidget* emailEntry = nullptr;
    GtkWidget* passwordEntry = nullptr;
    GtkWidget* twoFaEntry = nullptr;
    GtkWidget* loginButton = nullptr;
    GtkWidget* loginStatus = nullptr;
    GtkWidget* loginSpinner = nullptr;

    // Browser view
    GtkWidget* pathLabel = nullptr;
    GtkWidget* backButton = nullptr;
    GtkWidget* listBox = nullptr;
    GtkWidget* statusBar = nullptr;
    GtkWidget* busySpinner = nullptr;
    GtkWidget* emptyTrashButton = nullptr;

    proton::Client client;
    std::vector<PathEntry> path;          // navigation stack
    std::vector<proton::Item> items;      // current folder contents
};

App* g_app = nullptr;

// ---- main-loop dispatch helper -------------------------------------------
struct IdleMsg { std::function<void()> fn; };
gboolean idleTrampoline(gpointer data) {
    auto* m = static_cast<IdleMsg*>(data);
    m->fn();
    delete m;
    return G_SOURCE_REMOVE;
}
void postToMain(std::function<void()> fn) {
    g_idle_add(idleTrampoline, new IdleMsg{std::move(fn)});
}

std::string humanSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double s = (double)bytes;
    int u = 0;
    while (s >= 1024.0 && u < 4) { s /= 1024.0; ++u; }
    char buf[64];
    if (u == 0) snprintf(buf, sizeof(buf), "%llu %s", (unsigned long long)bytes, units[u]);
    else snprintf(buf, sizeof(buf), "%.1f %s", s, units[u]);
    return buf;
}

void setStatus(const std::string& text) {
    if (g_app->statusBar) gtk_label_set_text(GTK_LABEL(g_app->statusBar), text.c_str());
}

// ---- forward declarations -------------------------------------------------
void loadFolder(const std::string& linkId);
void rebuildPathLabel();

// ---- file download --------------------------------------------------------
struct SaveCtx { proton::Item item; };

void onSaveReady(GObject* source, GAsyncResult* res, gpointer user_data) {
    std::unique_ptr<SaveCtx> ctx(static_cast<SaveCtx*>(user_data));
    GError* err = nullptr;
    GFile* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, &err);
    if (!file) {
        if (err) g_error_free(err);
        return; // cancelled
    }
    char* cpath = g_file_get_path(file);
    std::string dest = cpath ? cpath : "";
    g_free(cpath);
    g_object_unref(file);
    if (dest.empty()) return;

    proton::Item item = ctx->item;
    setStatus("Downloading " + item.name + " ...");
    gtk_widget_set_sensitive(g_app->listBox, FALSE);

    std::thread([item, dest]() {
        std::string err;
        try {
            g_app->client.downloadFile(item, dest,
                [item](uint64_t done, uint64_t total) {
                    int pct = total ? (int)((done * 100) / total) : 100;
                    postToMain([item, pct]() {
                        setStatus("Downloading " + item.name + " ... " + std::to_string(pct) + "%");
                    });
                });
        } catch (const std::exception& e) {
            err = e.what();
        }
        postToMain([item, dest, err]() {
            gtk_widget_set_sensitive(g_app->listBox, TRUE);
            if (err.empty()) setStatus("Saved " + item.name + " to " + dest);
            else setStatus("Download failed: " + err);
        });
    }).detach();
}

void downloadItem(const proton::Item& item) {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save file as");
    gtk_file_dialog_set_initial_name(dialog, item.name.c_str());
    auto* ctx = new SaveCtx{item};
    gtk_file_dialog_save(dialog, GTK_WINDOW(g_app->window), nullptr,
                         onSaveReady, ctx);
    g_object_unref(dialog);
}

// ---- list rows ------------------------------------------------------------
void onRowActivated(GtkListBox*, GtkListBoxRow* row, gpointer) {
    int idx = gtk_list_box_row_get_index(row);
    if (idx < 0 || idx >= (int)g_app->items.size()) return;
    const proton::Item& item = g_app->items[idx];
    if (item.isFolder) {
        g_app->path.push_back({item.linkId, item.name});
        loadFolder(item.linkId);
    }
}

void onDownloadClicked(GtkButton*, gpointer) {
    GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_app->listBox));
    if (!row) {
        setStatus("Select a file to download first.");
        return;
    }
    int idx = gtk_list_box_row_get_index(row);
    if (idx < 0 || idx >= (int)g_app->items.size()) return;
    const proton::Item& item = g_app->items[idx];
    if (item.isFolder) {
        setStatus("Folders cannot be downloaded — open it and pick a file.");
        return;
    }
    downloadItem(item);
}

GtkWidget* makeRow(const proton::Item& item) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    const char* iconName = item.isFolder ? "folder-symbolic" : "text-x-generic-symbolic";
    GtkWidget* icon = gtk_image_new_from_icon_name(iconName);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 24);
    gtk_box_append(GTK_BOX(box), icon);

    GtkWidget* name = gtk_label_new(item.name.c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(box), name);

    if (!item.isFolder) {
        GtkWidget* size = gtk_label_new(humanSize(item.size).c_str());
        gtk_label_set_xalign(GTK_LABEL(size), 1.0);
        gtk_widget_add_css_class(size, "dim-label");
        gtk_box_append(GTK_BOX(box), size);
    }
    return box;
}

void populateList() {
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(g_app->listBox)) != nullptr)
        gtk_list_box_remove(GTK_LIST_BOX(g_app->listBox), child);

    for (const auto& item : g_app->items) {
        GtkWidget* row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), makeRow(item));
        gtk_list_box_append(GTK_LIST_BOX(g_app->listBox), row);
    }

    size_t folders = 0, files = 0;
    for (const auto& it : g_app->items) (it.isFolder ? folders : files)++;
    setStatus(std::to_string(folders) + " folders, " + std::to_string(files) + " files");
}

void rebuildPathLabel() {
    std::string p = "Home";
    for (size_t i = 1; i < g_app->path.size(); ++i) p += " / " + g_app->path[i].name;
    gtk_label_set_text(GTK_LABEL(g_app->pathLabel), p.c_str());
    gtk_widget_set_sensitive(g_app->backButton, g_app->path.size() > 1);
}

void loadFolder(const std::string& linkId) {
    rebuildPathLabel();
    setStatus("Loading ...");
    gtk_spinner_start(GTK_SPINNER(g_app->busySpinner));
    gtk_widget_set_sensitive(g_app->listBox, FALSE);

    std::thread([linkId]() {
        std::vector<proton::Item> items;
        std::string err;
        try {
            items = g_app->client.listDir(linkId);
        } catch (const std::exception& e) {
            err = e.what();
        }
        postToMain([items, err]() {
            gtk_spinner_stop(GTK_SPINNER(g_app->busySpinner));
            gtk_widget_set_sensitive(g_app->listBox, TRUE);
            if (!err.empty()) {
                setStatus("Error: " + err);
                return;
            }
            g_app->items = items;
            std::sort(g_app->items.begin(), g_app->items.end(),
                      [](const proton::Item& a, const proton::Item& b) {
                          if (a.isFolder != b.isFolder) return a.isFolder;
                          return g_ascii_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
                      });
            populateList();
        });
    }).detach();
}

void onBackClicked(GtkButton*, gpointer) {
    if (g_app->path.size() <= 1) return;
    g_app->path.pop_back();
    loadFolder(g_app->path.back().linkId);
}

void onRefreshClicked(GtkButton*, gpointer) {
    if (!g_app->path.empty()) loadFolder(g_app->path.back().linkId);
}

// ---- file upload ----------------------------------------------------------
void onOpenReady(GObject* source, GAsyncResult* res, gpointer) {
    GError* err = nullptr;
    GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &err);
    if (!file) {
        if (err) g_error_free(err);
        return; // cancelled
    }
    char* cpath = g_file_get_path(file);
    std::string src = cpath ? cpath : "";
    g_free(cpath);
    g_object_unref(file);
    if (src.empty() || g_app->path.empty()) return;

    std::string parent = g_app->path.back().linkId;
    std::string base = src;
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);

    setStatus("Uploading " + base + " ...");
    gtk_widget_set_sensitive(g_app->listBox, FALSE);

    std::thread([parent, src, base]() {
        std::string err;
        try {
            g_app->client.uploadFile(parent, src,
                [base](uint64_t done, uint64_t total) {
                    int pct = total ? (int)((done * 100) / total) : 100;
                    postToMain([base, pct]() {
                        setStatus("Uploading " + base + " ... " + std::to_string(pct) + "%");
                    });
                });
        } catch (const std::exception& e) {
            err = e.what();
        }
        postToMain([base, err, parent]() {
            gtk_widget_set_sensitive(g_app->listBox, TRUE);
            if (err.empty()) {
                setStatus("Uploaded " + base);
                if (!g_app->path.empty() && g_app->path.back().linkId == parent)
                    loadFolder(parent);
            } else {
                setStatus("Upload failed: " + err);
            }
        });
    }).detach();
}

void onUploadClicked(GtkButton*, gpointer) {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose a file to upload");
    gtk_file_dialog_open(dialog, GTK_WINDOW(g_app->window), nullptr, onOpenReady, nullptr);
    g_object_unref(dialog);
}

// ---- delete (move to trash) ----------------------------------------------
void doTrash(const proton::Item& item) {
    std::string parent = g_app->path.empty() ? "" : g_app->path.back().linkId;
    setStatus("Moving " + item.name + " to trash ...");
    gtk_widget_set_sensitive(g_app->listBox, FALSE);
    std::thread([item, parent]() {
        std::string err;
        try {
            g_app->client.trashItem(item);
        } catch (const std::exception& e) {
            err = e.what();
        }
        postToMain([item, err, parent]() {
            gtk_widget_set_sensitive(g_app->listBox, TRUE);
            if (err.empty()) {
                setStatus("Moved " + item.name + " to trash");
                if (!g_app->path.empty() && g_app->path.back().linkId == parent)
                    loadFolder(parent);
            } else {
                setStatus("Delete failed: " + err);
            }
        });
    }).detach();
}

void onTrashConfirm(GObject* source, GAsyncResult* res, gpointer user_data) {
    int* idxPtr = static_cast<int*>(user_data);
    int idx = *idxPtr;
    delete idxPtr;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, nullptr);
    if (choice == 1 && idx >= 0 && idx < (int)g_app->items.size())
        doTrash(g_app->items[idx]);
}

void onDeleteClicked(GtkButton*, gpointer) {
    GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_app->listBox));
    if (!row) {
        setStatus("Select an item to delete first.");
        return;
    }
    int idx = gtk_list_box_row_get_index(row);
    if (idx < 0 || idx >= (int)g_app->items.size()) return;
    const proton::Item& item = g_app->items[idx];

    GtkAlertDialog* dlg = gtk_alert_dialog_new(
        "Move \"%s\" to the trash?", item.name.c_str());
    gtk_alert_dialog_set_detail(dlg,
        item.isFolder ? "The folder and its contents will be moved to the Proton Drive trash."
                      : "The file will be moved to the Proton Drive trash.");
    const char* buttons[] = {"Cancel", "Move to Trash", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 1);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(g_app->window), nullptr,
                            onTrashConfirm, new int(idx));
    g_object_unref(dlg);
}

// ---- empty trash (permanent) ---------------------------------------------
void doEmptyTrash() {
    setStatus("Emptying trash ...");
    gtk_widget_set_sensitive(g_app->emptyTrashButton, FALSE);
    std::thread([]() {
        std::string err;
        try {
            g_app->client.emptyTrash();
        } catch (const std::exception& e) {
            err = e.what();
        }
        postToMain([err]() {
            gtk_widget_set_sensitive(g_app->emptyTrashButton, TRUE);
            if (err.empty()) setStatus("Trash emptied.");
            else setStatus("Empty trash failed: " + err);
        });
    }).detach();
}

void onEmptyTrashConfirm(GObject* source, GAsyncResult* res, gpointer) {
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, nullptr);
    if (choice == 1) doEmptyTrash();
}

void onEmptyTrashClicked(GtkButton*, gpointer) {
    GtkAlertDialog* dlg = gtk_alert_dialog_new("Permanently delete all trashed items?");
    gtk_alert_dialog_set_detail(dlg,
        "This empties the Proton Drive trash. The items cannot be recovered.");
    const char* buttons[] = {"Cancel", "Delete Forever", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(g_app->window), nullptr,
                            onEmptyTrashConfirm, nullptr);
    g_object_unref(dlg);
}

// ---- about dialog ---------------------------------------------------------
void showAbout(GSimpleAction*, GVariant*, gpointer) {
    GtkWidget* dlg = gtk_about_dialog_new();
    GtkAboutDialog* about = GTK_ABOUT_DIALOG(dlg);
    gtk_about_dialog_set_program_name(about, "Proton Drive GUI");
    gtk_about_dialog_set_version(about, APP_VERSION);
    gtk_about_dialog_set_logo_icon_name(about, APP_ID);
    gtk_about_dialog_set_comments(about,
        "A desktop client to manage your Proton Drive files.");
    const char* authors[] = {"Jean-Francois Lachance-Caumartin", nullptr};
    gtk_about_dialog_set_authors(about, authors);
    const char* credits[] = {"Proton AG", nullptr};
    gtk_about_dialog_add_credit_section(about, "Credits", credits);
    gtk_about_dialog_set_license_type(about, GTK_LICENSE_MIT_X11);
    if (g_app->window)
        gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(g_app->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_present(GTK_WINDOW(dlg));
}

void onLogout(GSimpleAction*, GVariant*, gpointer) {
    g_app->client.logout();
    g_app->path.clear();
    g_app->items.clear();
    gtk_stack_set_visible_child_name(GTK_STACK(g_app->stack), "login");
    gtk_label_set_text(GTK_LABEL(g_app->loginStatus), "");
}

// ---- login ----------------------------------------------------------------
void doLogin(GtkWidget*, gpointer) {
    const char* email = gtk_editable_get_text(GTK_EDITABLE(g_app->emailEntry));
    const char* password = gtk_editable_get_text(GTK_EDITABLE(g_app->passwordEntry));
    const char* twofa = gtk_editable_get_text(GTK_EDITABLE(g_app->twoFaEntry));
    std::string e = email ? email : "", p = password ? password : "", t = twofa ? twofa : "";
    if (e.empty() || p.empty()) {
        gtk_label_set_text(GTK_LABEL(g_app->loginStatus), "Please enter email and password.");
        return;
    }
    gtk_widget_set_sensitive(g_app->loginButton, FALSE);
    gtk_label_set_text(GTK_LABEL(g_app->loginStatus), "Signing in ...");
    gtk_spinner_start(GTK_SPINNER(g_app->loginSpinner));

    std::thread([e, p, t]() {
        std::string err;
        try {
            g_app->client.login(e, p, t);
        } catch (const std::exception& ex) {
            err = ex.what();
        }
        postToMain([err]() {
            gtk_spinner_stop(GTK_SPINNER(g_app->loginSpinner));
            gtk_widget_set_sensitive(g_app->loginButton, TRUE);
            if (!err.empty()) {
                gtk_label_set_text(GTK_LABEL(g_app->loginStatus), ("Login failed: " + err).c_str());
                return;
            }
            gtk_label_set_text(GTK_LABEL(g_app->loginStatus), "");
            gtk_stack_set_visible_child_name(GTK_STACK(g_app->stack), "browser");
            g_app->path.clear();
            g_app->path.push_back({g_app->client.rootLinkId(), "Home"});
            loadFolder(g_app->client.rootLinkId());
        });
    }).detach();
}

// ---- UI construction ------------------------------------------------------
GtkWidget* buildLoginView() {
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(outer, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(card, 340, -1);
    gtk_widget_set_margin_top(card, 24);
    gtk_widget_set_margin_bottom(card, 24);
    gtk_widget_set_margin_start(card, 24);
    gtk_widget_set_margin_end(card, 24);

    GtkWidget* logo = gtk_image_new_from_icon_name(APP_ID);
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 72);
    gtk_box_append(GTK_BOX(card), logo);

    GtkWidget* title = gtk_label_new("Sign in to Proton Drive");
    gtk_widget_add_css_class(title, "title-2");
    gtk_box_append(GTK_BOX(card), title);

    g_app->emailEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_app->emailEntry), "Email");
    gtk_entry_set_input_purpose(GTK_ENTRY(g_app->emailEntry), GTK_INPUT_PURPOSE_EMAIL);
    gtk_box_append(GTK_BOX(card), g_app->emailEntry);

    g_app->passwordEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_app->passwordEntry), "Password");
    gtk_entry_set_visibility(GTK_ENTRY(g_app->passwordEntry), FALSE);
    gtk_box_append(GTK_BOX(card), g_app->passwordEntry);

    g_app->twoFaEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_app->twoFaEntry),
                                   "Two-factor code (if enabled)");
    gtk_box_append(GTK_BOX(card), g_app->twoFaEntry);

    g_app->loginButton = gtk_button_new_with_label("Sign in");
    gtk_widget_add_css_class(g_app->loginButton, "suggested-action");
    g_signal_connect(g_app->loginButton, "clicked", G_CALLBACK(doLogin), nullptr);
    gtk_box_append(GTK_BOX(card), g_app->loginButton);

    g_signal_connect(g_app->passwordEntry, "activate", G_CALLBACK(doLogin), nullptr);
    g_signal_connect(g_app->twoFaEntry, "activate", G_CALLBACK(doLogin), nullptr);

    GtkWidget* spinnerRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(spinnerRow, GTK_ALIGN_CENTER);
    g_app->loginSpinner = gtk_spinner_new();
    gtk_box_append(GTK_BOX(spinnerRow), g_app->loginSpinner);
    g_app->loginStatus = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(g_app->loginStatus), TRUE);
    gtk_box_append(GTK_BOX(spinnerRow), g_app->loginStatus);
    gtk_box_append(GTK_BOX(card), spinnerRow);

    gtk_box_append(GTK_BOX(outer), card);
    return outer;
}

GtkWidget* buildBrowserView() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(toolbar, 6);
    gtk_widget_set_margin_bottom(toolbar, 6);
    gtk_widget_set_margin_start(toolbar, 6);
    gtk_widget_set_margin_end(toolbar, 6);

    g_app->backButton = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_set_tooltip_text(g_app->backButton, "Back");
    g_signal_connect(g_app->backButton, "clicked", G_CALLBACK(onBackClicked), nullptr);
    gtk_box_append(GTK_BOX(toolbar), g_app->backButton);

    GtkWidget* refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, "Refresh");
    g_signal_connect(refresh, "clicked", G_CALLBACK(onRefreshClicked), nullptr);
    gtk_box_append(GTK_BOX(toolbar), refresh);

    GtkWidget* download = gtk_button_new_from_icon_name("document-save-symbolic");
    gtk_widget_set_tooltip_text(download, "Download the selected file");
    g_signal_connect(download, "clicked", G_CALLBACK(onDownloadClicked), nullptr);
    gtk_box_append(GTK_BOX(toolbar), download);

    GtkWidget* upload = gtk_button_new_from_icon_name("document-send-symbolic");
    gtk_widget_set_tooltip_text(upload, "Upload a file to this folder");
    g_signal_connect(upload, "clicked", G_CALLBACK(onUploadClicked), nullptr);
    gtk_box_append(GTK_BOX(toolbar), upload);

    GtkWidget* del = gtk_button_new_from_icon_name("edit-delete-symbolic");
    gtk_widget_set_tooltip_text(del, "Move the selected item to trash");
    g_signal_connect(del, "clicked", G_CALLBACK(onDeleteClicked), nullptr);
    gtk_box_append(GTK_BOX(toolbar), del);

    g_app->emptyTrashButton = gtk_button_new_from_icon_name("user-trash-full-symbolic");
    gtk_widget_set_tooltip_text(g_app->emptyTrashButton,
                                "Empty the trash (permanently delete)");
    gtk_widget_add_css_class(g_app->emptyTrashButton, "destructive-action");
    g_signal_connect(g_app->emptyTrashButton, "clicked",
                     G_CALLBACK(onEmptyTrashClicked), nullptr);
    gtk_box_append(GTK_BOX(toolbar), g_app->emptyTrashButton);

    g_app->pathLabel = gtk_label_new("Home");
    gtk_label_set_xalign(GTK_LABEL(g_app->pathLabel), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(g_app->pathLabel), PANGO_ELLIPSIZE_START);
    gtk_widget_set_hexpand(g_app->pathLabel, TRUE);
    gtk_box_append(GTK_BOX(toolbar), g_app->pathLabel);

    g_app->busySpinner = gtk_spinner_new();
    gtk_box_append(GTK_BOX(toolbar), g_app->busySpinner);

    gtk_box_append(GTK_BOX(box), toolbar);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    g_app->listBox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_app->listBox), GTK_SELECTION_SINGLE);
    g_signal_connect(g_app->listBox, "row-activated", G_CALLBACK(onRowActivated), nullptr);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), g_app->listBox);
    gtk_box_append(GTK_BOX(box), scrolled);

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    g_app->statusBar = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_app->statusBar), 0.0);
    gtk_widget_set_margin_top(g_app->statusBar, 4);
    gtk_widget_set_margin_bottom(g_app->statusBar, 4);
    gtk_widget_set_margin_start(g_app->statusBar, 8);
    gtk_widget_set_margin_end(g_app->statusBar, 8);
    gtk_box_append(GTK_BOX(box), g_app->statusBar);

    return box;
}

void onActivate(GtkApplication* app, gpointer) {
    g_app->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_app->window), "Proton Drive GUI");
    gtk_window_set_default_size(GTK_WINDOW(g_app->window), 760, 560);
    gtk_window_set_icon_name(GTK_WINDOW(g_app->window), APP_ID);

    GtkWidget* header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(g_app->window), header);

    GMenu* menu = g_menu_new();
    g_menu_append(menu, "Sign out", "app.logout");
    g_menu_append(menu, "About Proton Drive GUI", "app.about");
    GtkWidget* menuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menuButton), G_MENU_MODEL(menu));
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menuButton);

    g_app->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(g_app->stack), buildLoginView(), "login");
    gtk_stack_add_named(GTK_STACK(g_app->stack), buildBrowserView(), "browser");
    gtk_stack_set_visible_child_name(GTK_STACK(g_app->stack), "login");

    gtk_window_set_child(GTK_WINDOW(g_app->window), g_app->stack);
    gtk_window_present(GTK_WINDOW(g_app->window));
}

void onStartup(GtkApplication* app, gpointer) {
    GSimpleAction* about = g_simple_action_new("about", nullptr);
    g_signal_connect(about, "activate", G_CALLBACK(showAbout), nullptr);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about));

    GSimpleAction* logout = g_simple_action_new("logout", nullptr);
    g_signal_connect(logout, "activate", G_CALLBACK(onLogout), nullptr);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(logout));
}

} // namespace

int main(int argc, char** argv) {
    App app;
    g_app = &app;
    g_set_application_name("Proton Drive GUI");

    app.app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.app, "startup", G_CALLBACK(onStartup), nullptr);
    g_signal_connect(app.app, "activate", G_CALLBACK(onActivate), nullptr);
    int status = g_application_run(G_APPLICATION(app.app), argc, argv);
    g_object_unref(app.app);
    return status;
}
