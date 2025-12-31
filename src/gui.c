#include <gtk/gtk.h>
#include <vte/vte.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <linux/limits.h>
#include <sys/statvfs.h>
#include <dirent.h>

// Global paths - will be set at runtime based on executable location
static char g_config_file[PATH_MAX];
static char g_sandbox_bin[PATH_MAX];
static char g_log_file[PATH_MAX];

// Macros for compatibility with existing code
#define CONFIG_FILE g_config_file
#define SANDBOX_BIN g_sandbox_bin
#define LOG_FILE g_log_file

// System resource info (detected at startup)
static int g_system_cpu_cores = 4;
static long g_system_total_memory_mb = 4096;
static long g_system_available_memory_mb = 2048;

typedef struct {
    char name[256];
    int memory;      // MB
    int cpu_cores;   // Number of CPU cores (was: cpu time in seconds)
    int network;
    time_t date;
} Sandbox;

GtkWidget *entry_name;
GtkWidget *scale_memory;
GtkWidget *spin_memory;
GtkWidget *label_memory_info;
GtkWidget *scale_cpu;
GtkWidget *spin_cpu;
GtkWidget *label_cpu_info;
GtkWidget *check_network;
GtkWidget *listbox;
GtkWidget *log_view;
GQueue *log_buffer = NULL;
GList *sandboxes = NULL;

// New UI widgets
GtkWidget *sys_cpu_bar;
GtkWidget *sys_mem_bar;
GtkWidget *sys_uptime_label;
GtkWidget *sandbox_count_label;
GtkWidget *detail_name_label;
GtkWidget *detail_memory_label;
GtkWidget *detail_cpu_label;
GtkWidget *detail_network_label;
GtkWidget *detail_created_label;
GtkWidget *detail_panel;
GtkWidget *status_bar;

// File Explorer widgets
GtkWidget *file_explorer_sandbox_combo;
GtkWidget *file_path_entry;
GtkWidget *file_tree_view;
GtkListStore *file_list_store;
GtkWidget *file_dir_tree;
GtkTreeStore *dir_tree_store;
static char current_file_path[PATH_MAX] = "/";

// Process Manager widgets  
GtkWidget *process_sandbox_combo;
GtkWidget *process_tree_view;
GtkListStore *process_list_store;
GtkWidget *process_auto_refresh_check;
guint process_refresh_timer = 0;

typedef struct {
    GtkWindow *window;
    char *sandbox_name;
} TerminalContext;

typedef struct {
    GtkWidget *row;
    GtkWidget *name_label;
    GtkWidget *mem_bar;
    GtkWidget *cpu_bar;
    GtkWidget *net_label;
    GtkWidget *status_label;
    GtkWidget *proc_count_label;
    Sandbox *sandbox;
} RowWidgets;

static gboolean refresh_usage_cb(gpointer user_data);
static gboolean get_usage_for(const char *name, double *cpu, double *mem);
static void on_terminal_mapped(GtkWidget *terminal, gpointer user_data);
static void on_spawn_ready(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data);
static void show_error_dialog(GtkWindow *parent, const char *msg, GError *err);
static void free_terminal_ctx(gpointer data);
static void log_gui_event(const char *level, const char *sandbox, const char *message);
static void update_log_view(void);
static gboolean refresh_system_info_cb(gpointer user_data);
static void update_sandbox_details(Sandbox *s);
static void on_listbox_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void apply_css_styling(void);
static void update_status_bar(const char *message);
static void on_refresh_clicked(GtkButton *button, gpointer user_data);
static void on_export_logs_clicked(GtkButton *button, gpointer user_data);
static void on_about_clicked(GtkButton *button, gpointer user_data);

// New function declarations for resource controls
static void on_memory_slider_changed(GtkRange *range, gpointer user_data);
static void on_memory_spin_changed(GtkSpinButton *spin, gpointer user_data);
static void on_cpu_slider_changed(GtkRange *range, gpointer user_data);
static void on_cpu_spin_changed(GtkSpinButton *spin, gpointer user_data);
static void update_memory_info_label(void);
static void update_cpu_info_label(void);
static void on_template_dev_clicked(GtkButton *button, gpointer user_data);
static void on_template_secure_clicked(GtkButton *button, gpointer user_data);
static void on_template_testing_clicked(GtkButton *button, gpointer user_data);

// File Explorer function declarations
static void refresh_file_list(const char *sandbox_name, const char *path);
static void on_file_go_clicked(GtkButton *button, gpointer user_data);
static void on_file_up_clicked(GtkButton *button, gpointer user_data);
static void on_file_refresh_clicked(GtkButton *button, gpointer user_data);
static void on_file_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data);
static void on_file_upload_clicked(GtkButton *button, gpointer user_data);
static void on_file_download_clicked(GtkButton *button, gpointer user_data);
static void on_file_delete_clicked(GtkButton *button, gpointer user_data);
static void on_file_new_folder_clicked(GtkButton *button, gpointer user_data);
static GtkWidget *create_file_explorer_tab(void);

// Process Manager function declarations
static void refresh_process_list(const char *sandbox_name);
static void on_process_kill_clicked(GtkButton *button, gpointer user_data);
static void on_process_refresh_clicked(GtkButton *button, gpointer user_data);
static gboolean on_process_auto_refresh(gpointer user_data);
static void on_process_auto_toggle(GtkToggleButton *button, gpointer user_data);
static GtkWidget *create_process_manager_tab(void);

// Detect system resources at startup
static void detect_system_resources(void) {
    // Detect CPU cores
    g_system_cpu_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (g_system_cpu_cores <= 0) g_system_cpu_cores = 4;
    
    // Detect total and available memory
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        g_system_total_memory_mb = (pages * page_size) / (1024 * 1024);
    }
    
    // Get available memory from /proc/meminfo
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        long mem_available = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemAvailable:", 13) == 0) {
                sscanf(line + 13, "%ld", &mem_available);
                g_system_available_memory_mb = mem_available / 1024;
                break;
            }
        }
        fclose(f);
    }
    
    // Fallback if MemAvailable not found
    if (g_system_available_memory_mb <= 0) {
        g_system_available_memory_mb = g_system_total_memory_mb / 2;
    }
}

void load_sandboxes() {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        Sandbox *s = malloc(sizeof(Sandbox));
        sscanf(line, "%s %d %d %d %ld", s->name, &s->memory, &s->cpu_cores, &s->network, &s->date);
        sandboxes = g_list_append(sandboxes, s);
    }
    fclose(f);
}

void save_sandboxes() {
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    for (GList *l = sandboxes; l; l = l->next) {
        Sandbox *s = l->data;
        fprintf(f, "%s %d %d %d %ld\n", s->name, s->memory, s->cpu_cores, s->network, s->date);
    }
    fclose(f);
}

static gboolean ensure_root(GtkWindow *parent) {
    if (geteuid() == 0) return TRUE;
    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_OK,
                                               "This action requires root privileges.\nRun with sudo or configure polkit.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return FALSE;
}

static gboolean run_command(char *const argv[], GtkWindow *parent) {
    GError *err = NULL;
    gchar *stdout_str = NULL;
    gchar *stderr_str = NULL;
    gint status = 0;
    gboolean ok = g_spawn_sync(NULL,
                               (gchar **)argv,
                               NULL,
                               G_SPAWN_SEARCH_PATH,
                               NULL,
                               NULL,
                               &stdout_str,
                               &stderr_str,
                               &status,
                               &err);
    if (!ok || status != 0) {
        show_error_dialog(parent, err ? "Command failed to start" : "Command returned error", err);
        if (stderr_str && *stderr_str) {
            g_printerr("%s\n", stderr_str);
        }
        g_clear_error(&err);
    }
    g_free(stdout_str);
    g_free(stderr_str);
    return ok && status == 0;
}

void update_list() {
    // Clear listbox
    GList *children = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = children; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    for (GList *l = sandboxes; l; l = l->next) {
        Sandbox *s = l->data;
        char buf[512];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&s->date));
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        GtkWidget *name = gtk_label_new(s->name);
        gtk_box_pack_start(GTK_BOX(row_box), name, FALSE, FALSE, 0);

        GtkWidget *mem_bar = gtk_progress_bar_new();
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(mem_bar), "Memory: N/A");
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(mem_bar), TRUE);
        gtk_widget_set_hexpand(mem_bar, TRUE);
        gtk_box_pack_start(GTK_BOX(row_box), mem_bar, TRUE, TRUE, 0);

        GtkWidget *cpu_bar = gtk_progress_bar_new();
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(cpu_bar), "CPU: N/A");
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(cpu_bar), TRUE);
        gtk_widget_set_hexpand(cpu_bar, TRUE);
        gtk_box_pack_start(GTK_BOX(row_box), cpu_bar, TRUE, TRUE, 0);

        GtkWidget *net_label = gtk_label_new(s->network ? "Net: On" : "Net: Off");
        gtk_box_pack_start(GTK_BOX(row_box), net_label, FALSE, FALSE, 0);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), row_box);

        RowWidgets *rw = g_new0(RowWidgets, 1);
        rw->row = row;
        rw->name_label = name;
        rw->mem_bar = mem_bar;
        rw->cpu_bar = cpu_bar;
        rw->net_label = net_label;
        rw->sandbox = s;
        g_object_set_data_full(G_OBJECT(row), "row_widgets", rw, g_free);

        gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
    }
    gtk_widget_show_all(listbox);
}

static void show_error_dialog(GtkWindow *parent, const char *msg, GError *err) {
    g_printerr("%s: %s\n", msg, err ? err->message : "unknown error");
    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_OK,
                                               "%s", msg);
    if (err && err->message) {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", err->message);
    }
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void on_spawn_ready(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data) {
    (void)terminal; // Unused parameter
    (void)pid;      // Unused parameter
    TerminalContext *ctx = user_data;
    if (error) {
        show_error_dialog(ctx ? ctx->window : NULL, "Failed to start sandbox shell", error);
        log_gui_event("ERROR", ctx ? ctx->sandbox_name : NULL, error->message);
        if (ctx && ctx->window) {
            gtk_widget_destroy(GTK_WIDGET(ctx->window));
        }
        return;
    }
    (void)pid; // child-exited signal will close the window
    log_gui_event("INFO", ctx ? ctx->sandbox_name : NULL, "Spawned sandbox terminal");
}

static void on_terminal_mapped(GtkWidget *terminal, gpointer user_data) {
    TerminalContext *ctx = user_data;

    // Prevent double-spawn if the widget remaps
    if (g_object_get_data(G_OBJECT(terminal), "spawned")) {
        return;
    }
    g_object_set_data(G_OBJECT(terminal), "spawned", GINT_TO_POINTER(1));

    if (!ctx || !ctx->sandbox_name) {
        show_error_dialog(ctx ? ctx->window : NULL, "Sandbox name missing", NULL);
        if (ctx && ctx->window) {
            gtk_widget_destroy(GTK_WIDGET(ctx->window));
        }
        return;
    }

    const char *argv[] = {SANDBOX_BIN, "-e", "-s", ctx->sandbox_name, NULL};
    char **envv = g_get_environ();

    vte_terminal_spawn_async(VTE_TERMINAL(terminal),
                             VTE_PTY_DEFAULT,
                             NULL,               // inherit cwd
                             (char **)argv,
                             envv,               // inherit env
                             G_SPAWN_SEARCH_PATH,
                             NULL, NULL,         // child_setup
                             NULL,               // cancellable
                             -1,                 // default timeout
                             NULL,               // cancellable
                             on_spawn_ready,
                             ctx);

    g_strfreev(envv);
}

static void free_terminal_ctx(gpointer data) {
    TerminalContext *ctx = data;
    if (!ctx) return;
    g_free(ctx->sandbox_name);
    g_free(ctx);
}

static void update_log_view(void) {
    if (!log_view || !log_buffer) return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    gtk_text_buffer_set_text(buffer, "", -1);
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buffer, &iter);
    for (GList *l = log_buffer->head; l; l = l->next) {
        const char *line = l->data;
        gtk_text_buffer_insert(buffer, &iter, line, -1);
        gtk_text_buffer_insert(buffer, &iter, "\n", -1);
    }
}

static void log_gui_event(const char *level, const char *sandbox, const char *message) {
    if (!log_buffer) return;
    char ts[64];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    char line[512];
    snprintf(line, sizeof(line), "[%s] %s %s %s", ts, level ? level : "INFO", sandbox ? sandbox : "-", message ? message : "");

    g_queue_push_tail(log_buffer, g_strdup(line));
    while (g_queue_get_length(log_buffer) > 200) {
        char *old = g_queue_pop_head(log_buffer);
        g_free(old);
    }

    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    update_log_view();
}

void on_create_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry_name));
    int memory = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_memory));
    int cpu_cores = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_cpu));
    int network = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_network));

    if (!name || !*name) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Please enter a sandbox name");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    // Validate values against system limits
    if (memory < 64 || memory > g_system_total_memory_mb) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Memory must be between 64 MB and %ld MB", g_system_total_memory_mb);
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", msg);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (cpu_cores < 1 || cpu_cores > g_system_cpu_cores) {
        char msg[256];
        snprintf(msg, sizeof(msg), "CPU cores must be between 1 and %d", g_system_cpu_cores);
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", msg);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    // Prevent duplicate names
    for (GList *l = sandboxes; l; l = l->next) {
        Sandbox *s = l->data;
        if (strcmp(s->name, name) == 0) {
            GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "A sandbox with this name already exists");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            return;
        }
    }

    // Build argv dynamically - use new -p for CPU cores
    char *argv_cmd[12] = {0};
    int idx = 0;
    argv_cmd[idx++] = SANDBOX_BIN;
    argv_cmd[idx++] = "-c";
    argv_cmd[idx++] = "-m";
    char mem_buf[16];
    snprintf(mem_buf, sizeof(mem_buf), "%d", memory);
    argv_cmd[idx++] = mem_buf;
    argv_cmd[idx++] = "-p";
    char cpu_buf[16];
    snprintf(cpu_buf, sizeof(cpu_buf), "%d", cpu_cores);
    argv_cmd[idx++] = cpu_buf;
    if (network) {
        argv_cmd[idx++] = "-n";
    }
    argv_cmd[idx++] = "-s";
    argv_cmd[idx++] = (char *)name;
    argv_cmd[idx] = NULL;

    if (!run_command(argv_cmd, NULL)) {
        return;
    }

    // Add to list
    Sandbox *s = malloc(sizeof(Sandbox));
    strcpy(s->name, name);
    s->memory = memory;
    s->cpu_cores = cpu_cores;
    s->network = network;
    s->date = time(NULL);
    sandboxes = g_list_append(sandboxes, s);
    save_sandboxes();
    update_list();
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Created sandbox (%d MB, %d cores, %s)", 
             memory, cpu_cores, network ? "network" : "isolated");
    log_gui_event("INFO", name, log_msg);
    update_status_bar(log_msg);
}

void on_child_exited(VteTerminal *terminal, int status, gpointer user_data) {
    (void)terminal; // Unused
    (void)status;   // Unused
    gtk_widget_destroy(GTK_WIDGET(user_data));
}

void on_enter_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(listbox));
    if (!row) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Please select a sandbox to enter");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (!ensure_root(NULL)) return;

    RowWidgets *rw = g_object_get_data(G_OBJECT(row), "row_widgets");
    if (!rw || !rw->sandbox) return;
    char name[256];
    strncpy(name, rw->sandbox->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[300];
    snprintf(title, sizeof(title), "🔒 Sandbox Terminal - %s", name);
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 650);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), box);

    // Create terminal and pack it before interacting with it
    GtkWidget *terminal = vte_terminal_new();
    
    // Enhanced terminal styling
    // Set font
    PangoFontDescription *font_desc = pango_font_description_from_string("JetBrains Mono 12");
    if (!font_desc) font_desc = pango_font_description_from_string("Monospace 12");
    vte_terminal_set_font(VTE_TERMINAL(terminal), font_desc);
    pango_font_description_free(font_desc);
    
    // Dracula-inspired color scheme
    GdkRGBA fg_color = {0.97, 0.97, 0.95, 1.0};  // #f8f8f2
    GdkRGBA bg_color = {0.16, 0.16, 0.21, 1.0};  // #282a36
    vte_terminal_set_colors(VTE_TERMINAL(terminal), &fg_color, &bg_color, NULL, 0);
    
    // Additional terminal settings
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(terminal), 10000);
    vte_terminal_set_cursor_blink_mode(VTE_TERMINAL(terminal), VTE_CURSOR_BLINK_ON);
    vte_terminal_set_mouse_autohide(VTE_TERMINAL(terminal), TRUE);
    
    gtk_box_pack_start(GTK_BOX(box), terminal, TRUE, TRUE, 0);

    // Prepare context for spawn callbacks
    TerminalContext *ctx = g_new0(TerminalContext, 1);
    ctx->window = GTK_WINDOW(window);
    ctx->sandbox_name = g_strdup(name);
    g_object_set_data_full(G_OBJECT(window), "terminal_ctx", ctx, free_terminal_ctx);

    // Connect to child-exited to close window when process exits
    g_signal_connect(terminal, "child-exited", G_CALLBACK(on_child_exited), window);

    // Spawn only after the widget is mapped (has a GdkWindow)
    g_signal_connect(terminal, "map", G_CALLBACK(on_terminal_mapped), ctx);

    // Show all widgets after packing everything
    gtk_widget_show_all(window);
}

void on_clear_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    gtk_entry_set_text(GTK_ENTRY(entry_name), "");
    // Set sensible defaults
    int default_memory = g_system_total_memory_mb / 4;  // 25% of system memory
    if (default_memory < 256) default_memory = 256;
    if (default_memory > 4096) default_memory = 4096;
    int default_cores = g_system_cpu_cores / 2;
    if (default_cores < 1) default_cores = 1;
    
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_memory), default_memory);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_cpu), default_cores);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_network), FALSE);
}

void on_delete_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(listbox));
    if (!row) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Please select a sandbox to delete");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    RowWidgets *rw = g_object_get_data(G_OBJECT(row), "row_widgets");
    if (!rw || !rw->sandbox) return;
    char name[256];
    strncpy(name, rw->sandbox->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "Are you sure you want to delete the sandbox '%s'?", name);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_YES) {
        if (!ensure_root(NULL)) return;

        // Call delete
        char *argv_cmd[] = {SANDBOX_BIN, "-d", NULL};
        if (!run_command(argv_cmd, NULL)) {
            return;
        }

        // Remove from list
        for (GList *l = sandboxes; l; l = l->next) {
            Sandbox *s = l->data;
            if (strcmp(s->name, name) == 0) {
                sandboxes = g_list_remove(sandboxes, s);
                free(s);
                break;
            }
        }
        save_sandboxes();
        update_list();
        log_gui_event("INFO", name, "Deleted sandbox");
    }
}

static gboolean get_usage_for(const char *name, double *cpu_percent, double *mem_percent) {
    if (!name || !cpu_percent || !mem_percent) return FALSE;
    FILE *fp = popen("ps -eo pid,%cpu,%mem,cmd", "r");
    if (!fp) return FALSE;
    char line[2048];
    if (!fgets(line, sizeof(line), fp)) { // skip header
        pclose(fp);
        return FALSE;
    }
    gboolean found = FALSE;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, name)) continue;
        if (!(strstr(line, SANDBOX_BIN) || strstr(line, "-s"))) continue;
        double c = 0.0, m = 0.0;
        if (sscanf(line, "%*d %lf %lf", &c, &m) == 2) {
            *cpu_percent = c;
            *mem_percent = m;
            found = TRUE;
            break;
        }
    }
    // Note: rewind() doesn't work on pipes, so removed the broken fallback code
    // that attempted to rewind and rescan. If no sandbox process is found, 
    // we simply return FALSE.
    pclose(fp);
    return found;
}

static double system_total_mem_mb(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0.0;
    char key[64];
    long value_kb = 0;
    while (fscanf(fp, "%63s %ld kB\n", key, &value_kb) == 2) {
        if (strcmp(key, "MemTotal:") == 0) {
            fclose(fp);
            return value_kb / 1024.0;
        }
    }
    fclose(fp);
    return 0.0;
}

static gboolean refresh_usage_cb(gpointer user_data) {
    (void)user_data;
    double total_mem_mb = system_total_mem_mb();
    GList *rows = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *r = rows; r; r = r->next) {
        GtkListBoxRow *row = GTK_LIST_BOX_ROW(r->data);
        RowWidgets *rw = g_object_get_data(G_OBJECT(row), "row_widgets");
        if (!rw || !rw->sandbox) continue;
        double cpu = 0.0, mem_percent = 0.0;
        gboolean ok = get_usage_for(rw->sandbox->name, &cpu, &mem_percent);
        double mem_mb = 0.0;
        if (ok && total_mem_mb > 0.0) {
            mem_mb = mem_percent * total_mem_mb / 100.0;
        }
        // CPU bar
        if (ok) {
            double cpu_frac = cpu / 100.0;
            if (cpu_frac > 1.0) cpu_frac = 1.0;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(rw->cpu_bar), cpu_frac);
            char cpu_txt[64];
            snprintf(cpu_txt, sizeof(cpu_txt), "CPU: %.1f%%", cpu);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(rw->cpu_bar), cpu_txt);
        } else {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(rw->cpu_bar), 0.0);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(rw->cpu_bar), "CPU: N/A");
        }
        // Memory bar
        if (ok && total_mem_mb > 0.0) {
            double mem_frac = mem_percent / 100.0;
            if (mem_frac > 1.0) mem_frac = 1.0;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(rw->mem_bar), mem_frac);
            char mem_txt[64];
            snprintf(mem_txt, sizeof(mem_txt), "Mem: %.1f MB (%.1f%%)", mem_mb, mem_percent);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(rw->mem_bar), mem_txt);
        } else {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(rw->mem_bar), 0.0);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(rw->mem_bar), "Mem: N/A");
        }
        // Network label stays static
        gtk_label_set_text(GTK_LABEL(rw->net_label), rw->sandbox->network ? "Net: On" : "Net: Off");
    }
    g_list_free(rows);
    return TRUE;
}

// Initialize paths based on executable location
static void init_paths(const char *argv0) {
    char exe_path[PATH_MAX];
    char *dir;
    
    // Try to get the real path of the executable
    if (realpath(argv0, exe_path) == NULL) {
        // Fallback: try /proc/self/exe
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len != -1) {
            exe_path[len] = '\0';
        } else {
            // Last resort: use current directory
            getcwd(exe_path, sizeof(exe_path));
            strncat(exe_path, "/gui", sizeof(exe_path) - strlen(exe_path) - 1);
        }
    }
    
    // Get directory containing executable
    dir = dirname(exe_path);
    
    // Set paths relative to executable directory
    // Assuming structure: bin/gui, bin/sandbox, ../sandboxes.txt, ../gui.log
    snprintf(g_sandbox_bin, sizeof(g_sandbox_bin), "%s/sandbox", dir);
    
    // Go up one directory for config and log files
    char parent_dir[PATH_MAX];
    snprintf(parent_dir, sizeof(parent_dir), "%s/..", dir);
    
    // Use realpath with NULL to let it allocate the buffer (safer)
    char *resolved_parent = realpath(parent_dir, NULL);
    if (resolved_parent != NULL) {
        snprintf(g_config_file, sizeof(g_config_file), "%.4070s/sandboxes.txt", resolved_parent);
        snprintf(g_log_file, sizeof(g_log_file), "%.4080s/gui.log", resolved_parent);
        free(resolved_parent);
    } else {
        snprintf(g_config_file, sizeof(g_config_file), "%s/../sandboxes.txt", dir);
        snprintf(g_log_file, sizeof(g_log_file), "%s/../gui.log", dir);
    }
}

// Apply CSS styling - Modern Dark Theme with Glassmorphism
static void apply_css_styling(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css = 
        /* === MAIN WINDOW - Deep dark gradient === */
        "window { "
        "    background: linear-gradient(135deg, #0f0f1a 0%, #1a1a2e 50%, #16213e 100%); "
        "}"
        
        /* === FRAMES - Glassmorphism cards === */
        "frame { "
        "    background-color: rgba(255, 255, 255, 0.03); "
        "    border: 1px solid rgba(255, 255, 255, 0.1); "
        "    border-radius: 12px; "
        "}"
        "frame > label { "
        "    font-weight: bold; "
        "    color: #00d9ff; "
        "    font-size: 13px; "
        "    text-shadow: 0 0 10px rgba(0, 217, 255, 0.3); "
        "}"
        
        /* === LABELS - Clean white/cyan text === */
        "label { color: #e0e0e0; }"
        ".accent { color: #00d9ff; }"
        ".success { color: #22c55e; }"
        ".warning { color: #f59e0b; }"
        ".error { color: #ef4444; }"
        
        /* === BUTTONS - Glowing gradient === */
        "button { "
        "    background: linear-gradient(135deg, #1e3a5f 0%, #2d4a6f 100%); "
        "    border: 1px solid rgba(0, 217, 255, 0.3); "
        "    border-radius: 8px; "
        "    padding: 8px 16px; "
        "    color: #ffffff; "
        "    font-weight: 500; "
        "    transition: all 200ms ease; "
        "}"
        "button:hover { "
        "    background: linear-gradient(135deg, #00d9ff 0%, #a855f7 100%); "
        "    border-color: #00d9ff; "
        "    box-shadow: 0 0 20px rgba(0, 217, 255, 0.4); "
        "    color: #ffffff; "
        "}"
        "button:active { "
        "    background: linear-gradient(135deg, #00b8d9 0%, #9333ea 100%); "
        "}"
        "button:disabled { "
        "    background: #2a2a3a; "
        "    color: #666; "
        "    border-color: #444; "
        "}"
        
        /* === PRIMARY ACTION BUTTONS === */
        ".primary-button { "
        "    background: linear-gradient(135deg, #00d9ff 0%, #00b8d9 100%); "
        "    color: #0f0f1a; "
        "    font-weight: bold; "
        "}"
        ".primary-button:hover { "
        "    background: linear-gradient(135deg, #22e6ff 0%, #00d9ff 100%); "
        "    box-shadow: 0 0 25px rgba(0, 217, 255, 0.6); "
        "}"
        
        /* === DANGER BUTTONS === */
        ".danger-button { "
        "    background: linear-gradient(135deg, #dc2626 0%, #b91c1c 100%); "
        "    border-color: #ef4444; "
        "}"
        ".danger-button:hover { "
        "    background: linear-gradient(135deg, #ef4444 0%, #dc2626 100%); "
        "    box-shadow: 0 0 20px rgba(239, 68, 68, 0.5); "
        "}"
        
        /* === ENTRY FIELDS - Dark with glow === */
        "entry { "
        "    background-color: rgba(15, 15, 26, 0.8); "
        "    border: 1px solid rgba(255, 255, 255, 0.1); "
        "    border-radius: 8px; "
        "    padding: 10px 12px; "
        "    color: #ffffff; "
        "    caret-color: #00d9ff; "
        "}"
        "entry:focus { "
        "    border-color: #00d9ff; "
        "    box-shadow: 0 0 15px rgba(0, 217, 255, 0.3); "
        "}"
        "entry:disabled { background-color: #1a1a2e; color: #666; }"
        
        /* === PROGRESS BARS - Gradient fill === */
        "progressbar { "
        "    min-height: 12px; "
        "}"
        "progressbar trough { "
        "    background-color: rgba(255, 255, 255, 0.05); "
        "    border-radius: 6px; "
        "    border: 1px solid rgba(255, 255, 255, 0.1); "
        "}"
        "progressbar progress { "
        "    background: linear-gradient(90deg, #00d9ff 0%, #a855f7 100%); "
        "    border-radius: 6px; "
        "    box-shadow: 0 0 10px rgba(0, 217, 255, 0.5); "
        "}"
        
        /* === CPU PROGRESS BAR === */
        ".cpu-bar progress { "
        "    background: linear-gradient(90deg, #22c55e 0%, #eab308 50%, #ef4444 100%); "
        "}"
        
        /* === MEMORY PROGRESS BAR === */
        ".mem-bar progress { "
        "    background: linear-gradient(90deg, #a855f7 0%, #ec4899 100%); "
        "}"
        
        /* === SCALES/SLIDERS === */
        "scale { min-height: 20px; }"
        "scale trough { "
        "    background-color: rgba(255, 255, 255, 0.1); "
        "    border-radius: 10px; "
        "    min-height: 8px; "
        "}"
        "scale highlight { "
        "    background: linear-gradient(90deg, #00d9ff, #a855f7); "
        "    border-radius: 10px; "
        "}"
        "scale slider { "
        "    background: linear-gradient(135deg, #00d9ff, #00b8d9); "
        "    border-radius: 50%; "
        "    min-width: 20px; "
        "    min-height: 20px; "
        "    box-shadow: 0 0 10px rgba(0, 217, 255, 0.5); "
        "}"
        "scale slider:hover { "
        "    background: linear-gradient(135deg, #22e6ff, #00d9ff); "
        "    box-shadow: 0 0 15px rgba(0, 217, 255, 0.7); "
        "}"
        
        /* === SPIN BUTTONS === */
        "spinbutton { "
        "    background-color: rgba(15, 15, 26, 0.8); "
        "    border: 1px solid rgba(255, 255, 255, 0.1); "
        "    border-radius: 8px; "
        "    color: #ffffff; "
        "}"
        "spinbutton:focus { border-color: #00d9ff; }"
        "spinbutton button { "
        "    background: rgba(0, 217, 255, 0.2); "
        "    border: none; "
        "    color: #00d9ff; "
        "}"
        "spinbutton button:hover { background: rgba(0, 217, 255, 0.4); }"
        
        /* === NOTEBOOK TABS - Glowing active tab === */
        "notebook { background-color: transparent; }"
        "notebook header { "
        "    background-color: rgba(15, 15, 26, 0.5); "
        "    border-bottom: 1px solid rgba(255, 255, 255, 0.1); "
        "}"
        "notebook tab { "
        "    background-color: transparent; "
        "    padding: 10px 20px; "
        "    border: none; "
        "    color: #888; "
        "    font-weight: 500; "
        "}"
        "notebook tab:hover { "
        "    color: #00d9ff; "
        "    background-color: rgba(0, 217, 255, 0.1); "
        "}"
        "notebook tab:checked { "
        "    color: #00d9ff; "
        "    background: linear-gradient(180deg, rgba(0, 217, 255, 0.2), transparent); "
        "    border-bottom: 2px solid #00d9ff; "
        "    box-shadow: 0 2px 10px rgba(0, 217, 255, 0.3); "
        "}"
        "notebook stack { background-color: transparent; }"
        
        /* === LIST BOX - Dark rows with hover glow === */
        "list { "
        "    background-color: rgba(15, 15, 26, 0.5); "
        "    border-radius: 8px; "
        "}"
        "list row { "
        "    padding: 12px 16px; "
        "    border-bottom: 1px solid rgba(255, 255, 255, 0.05); "
        "    transition: all 150ms ease; "
        "}"
        "list row:hover { "
        "    background-color: rgba(0, 217, 255, 0.1); "
        "}"
        "list row:selected { "
        "    background: linear-gradient(90deg, rgba(0, 217, 255, 0.2), rgba(168, 85, 247, 0.2)); "
        "    border-left: 3px solid #00d9ff; "
        "}"
        
        /* === SCROLLED WINDOW === */
        "scrolledwindow { "
        "    background-color: rgba(15, 15, 26, 0.3); "
        "    border: 1px solid rgba(255, 255, 255, 0.05); "
        "    border-radius: 8px; "
        "}"
        "scrollbar { background-color: transparent; }"
        "scrollbar slider { "
        "    background-color: rgba(0, 217, 255, 0.3); "
        "    border-radius: 10px; "
        "    min-width: 8px; "
        "}"
        "scrollbar slider:hover { background-color: rgba(0, 217, 255, 0.5); }"
        
        /* === CHECK BUTTONS === */
        "checkbutton { color: #e0e0e0; }"
        "checkbutton check { "
        "    background-color: rgba(15, 15, 26, 0.8); "
        "    border: 2px solid rgba(255, 255, 255, 0.2); "
        "    border-radius: 4px; "
        "    min-width: 20px; "
        "    min-height: 20px; "
        "}"
        "checkbutton check:checked { "
        "    background: linear-gradient(135deg, #00d9ff, #a855f7); "
        "    border-color: #00d9ff; "
        "}"
        "checkbutton:hover check { border-color: #00d9ff; }"
        
        /* === SEPARATOR === */
        "separator { "
        "    background: linear-gradient(90deg, transparent, rgba(0, 217, 255, 0.3), transparent); "
        "    min-height: 1px; "
        "}"
        
        /* === STATUS BAR - Subtle gradient === */
        ".status-bar { "
        "    background: linear-gradient(90deg, rgba(0, 217, 255, 0.1), rgba(168, 85, 247, 0.1)); "
        "    color: #888; "
        "    padding: 8px 16px; "
        "    font-size: 11px; "
        "    border-top: 1px solid rgba(255, 255, 255, 0.05); "
        "}"
        
        /* === TEXT VIEW (Logs) - Terminal style === */
        "textview { "
        "    background-color: #0a0a12; "
        "    color: #22c55e; "
        "    font-family: 'JetBrains Mono', 'Fira Code', 'Consolas', monospace; "
        "}"
        "textview text { background-color: #0a0a12; }"
        
        /* === TREE VIEW (File Explorer) === */
        "treeview { "
        "    background-color: rgba(15, 15, 26, 0.5); "
        "    color: #e0e0e0; "
        "}"
        "treeview:selected { "
        "    background-color: rgba(0, 217, 255, 0.2); "
        "}"
        "treeview header button { "
        "    background: rgba(0, 217, 255, 0.1); "
        "    border: none; "
        "    color: #00d9ff; "
        "    font-weight: bold; "
        "}"
        
        /* === SANDBOX CARD STYLES === */
        ".sandbox-card { "
        "    background: rgba(255, 255, 255, 0.02); "
        "    border: 1px solid rgba(255, 255, 255, 0.08); "
        "    border-radius: 12px; "
        "    padding: 16px; "
        "}"
        ".sandbox-card:hover { "
        "    background: rgba(0, 217, 255, 0.05); "
        "    border-color: rgba(0, 217, 255, 0.3); "
        "}"
        
        /* === STATUS INDICATORS === */
        ".status-running { color: #22c55e; text-shadow: 0 0 10px rgba(34, 197, 94, 0.5); }"
        ".status-idle { color: #666; }"
        ".status-error { color: #ef4444; text-shadow: 0 0 10px rgba(239, 68, 68, 0.5); }"
        
        /* === HEADER TITLE === */
        ".app-title { "
        "    font-size: 24px; "
        "    font-weight: bold; "
        "    color: #00d9ff; "
        "    text-shadow: 0 0 20px rgba(0, 217, 255, 0.5); "
        "}"
        
        /* === PANED DIVIDER === */
        "paned separator { "
        "    background-color: rgba(0, 217, 255, 0.2); "
        "    min-width: 4px; "
        "}"
        "paned separator:hover { background-color: rgba(0, 217, 255, 0.5); }";
    
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}

// Get system uptime
static void get_system_uptime(char *buf, size_t len) {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) {
        snprintf(buf, len, "N/A");
        return;
    }
    double uptime_sec = 0;
    if (fscanf(f, "%lf", &uptime_sec) != 1) {
        snprintf(buf, len, "N/A");
        fclose(f);
        return;
    }
    fclose(f);
    
    int days = (int)(uptime_sec / 86400);
    int hours = (int)((uptime_sec - days * 86400) / 3600);
    int mins = (int)((uptime_sec - days * 86400 - hours * 3600) / 60);
    
    if (days > 0) {
        snprintf(buf, len, "%dd %dh %dm", days, hours, mins);
    } else if (hours > 0) {
        snprintf(buf, len, "%dh %dm", hours, mins);
    } else {
        snprintf(buf, len, "%dm", mins);
    }
}

// Get overall system CPU usage
static double get_system_cpu_usage(void) {
    static long prev_total = 0, prev_idle = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0.0;
    
    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0.0;
    }
    fclose(f);
    
    long user, nice, system, idle, iowait, irq, softirq;
    if (sscanf(line, "cpu %ld %ld %ld %ld %ld %ld %ld", 
               &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 7) {
        return 0.0;
    }
    
    long total = user + nice + system + idle + iowait + irq + softirq;
    long total_diff = total - prev_total;
    long idle_diff = idle - prev_idle;
    
    prev_total = total;
    prev_idle = idle;
    
    if (total_diff == 0) return 0.0;
    return 100.0 * (1.0 - (double)idle_diff / total_diff);
}

// Get system memory usage
static void get_system_memory(double *used_mb, double *total_mb, double *percent) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        *used_mb = *total_mb = *percent = 0;
        return;
    }
    
    long mem_total = 0, mem_free = 0, buffers = 0, cached = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) sscanf(line, "MemTotal: %ld", &mem_total);
        else if (strncmp(line, "MemFree:", 8) == 0) sscanf(line, "MemFree: %ld", &mem_free);
        else if (strncmp(line, "Buffers:", 8) == 0) sscanf(line, "Buffers: %ld", &buffers);
        else if (strncmp(line, "Cached:", 7) == 0) sscanf(line, "Cached: %ld", &cached);
    }
    fclose(f);
    
    long used = mem_total - mem_free - buffers - cached;
    *total_mb = mem_total / 1024.0;
    *used_mb = used / 1024.0;
    *percent = (mem_total > 0) ? 100.0 * used / mem_total : 0;
}

// Refresh system info callback
static gboolean refresh_system_info_cb(gpointer user_data) {
    (void)user_data;
    
    // Update CPU
    double cpu = get_system_cpu_usage();
    if (sys_cpu_bar) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(sys_cpu_bar), cpu / 100.0);
        char buf[64];
        snprintf(buf, sizeof(buf), "CPU: %.1f%%", cpu);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(sys_cpu_bar), buf);
    }
    
    // Update Memory
    double used_mb, total_mb, mem_percent;
    get_system_memory(&used_mb, &total_mb, &mem_percent);
    if (sys_mem_bar) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(sys_mem_bar), mem_percent / 100.0);
        char buf[64];
        snprintf(buf, sizeof(buf), "Mem: %.0f/%.0f MB (%.1f%%)", used_mb, total_mb, mem_percent);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(sys_mem_bar), buf);
    }
    
    // Update Uptime
    if (sys_uptime_label) {
        char uptime[64];
        get_system_uptime(uptime, sizeof(uptime));
        char buf[128];
        snprintf(buf, sizeof(buf), "Uptime: %s", uptime);
        gtk_label_set_text(GTK_LABEL(sys_uptime_label), buf);
    }
    
    // Update sandbox count
    if (sandbox_count_label) {
        int count = g_list_length(sandboxes);
        char buf[64];
        snprintf(buf, sizeof(buf), "Sandboxes: %d", count);
        gtk_label_set_text(GTK_LABEL(sandbox_count_label), buf);
    }
    
    return TRUE;
}

// Update sandbox detail panel
static void update_sandbox_details(Sandbox *s) {
    if (!detail_panel) return;
    
    if (!s) {
        gtk_widget_hide(detail_panel);
        return;
    }
    
    gtk_widget_show(detail_panel);
    
    char buf[512];  // Larger buffer for name + markup
    snprintf(buf, sizeof(buf), "<b>%s</b>", s->name);
    gtk_label_set_markup(GTK_LABEL(detail_name_label), buf);
    
    snprintf(buf, sizeof(buf), "Memory Limit: %d MB", s->memory);
    gtk_label_set_text(GTK_LABEL(detail_memory_label), buf);
    
    snprintf(buf, sizeof(buf), "CPU Cores: %d", s->cpu_cores);
    gtk_label_set_text(GTK_LABEL(detail_cpu_label), buf);
    
    gtk_label_set_text(GTK_LABEL(detail_network_label), 
                       s->network ? "Network: Enabled (Full Access)" : "Network: Disabled (Isolated)");
    
    char date_buf[64];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", localtime(&s->date));
    snprintf(buf, sizeof(buf), "Created: %s", date_buf);
    gtk_label_set_text(GTK_LABEL(detail_created_label), buf);
}

// Listbox row selected callback
static void on_listbox_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    (void)user_data;
    
    if (!row) {
        update_sandbox_details(NULL);
        return;
    }
    
    RowWidgets *rw = g_object_get_data(G_OBJECT(row), "row_widgets");
    if (rw && rw->sandbox) {
        update_sandbox_details(rw->sandbox);
    }
}

// Update status bar
static void update_status_bar(const char *message) {
    if (status_bar && message) {
        char buf[256];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%H:%M:%S", tm);
        snprintf(buf, sizeof(buf), "[%s] %s", ts, message);
        gtk_label_set_text(GTK_LABEL(status_bar), buf);
    }
}

// Refresh button callback
static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    // Reload sandboxes from file
    for (GList *l = sandboxes; l; l = l->next) {
        free(l->data);
    }
    g_list_free(sandboxes);
    sandboxes = NULL;
    
    load_sandboxes();
    update_list();
    update_status_bar("Sandbox list refreshed");
    log_gui_event("INFO", NULL, "Refreshed sandbox list");
}

// Export logs callback
static void on_export_logs_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Export Logs",
        NULL,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT,
        NULL
    );
    
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "sandbox_logs.txt");
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        FILE *f = fopen(filename, "w");
        if (f) {
            for (GList *l = log_buffer->head; l; l = l->next) {
                fprintf(f, "%s\n", (char*)l->data);
            }
            fclose(f);
            update_status_bar("Logs exported successfully");
            log_gui_event("INFO", NULL, "Exported logs to file");
        }
        g_free(filename);
    }
    
    gtk_widget_destroy(dialog);
}

// About dialog callback
static void on_about_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    char about_text[512];
    snprintf(about_text, sizeof(about_text),
             "Linux Sandbox Manager v2.0\n\n"
             "System: %d CPU cores, %ld MB RAM",
             g_system_cpu_cores, g_system_total_memory_mb);
    
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "%s", about_text
    );
    
    gtk_message_dialog_format_secondary_markup(
        GTK_MESSAGE_DIALOG(dialog),
        "<b>Features:</b>\n"
        "• Create isolated sandbox environments\n"
        "• PID, User, Network namespace isolation\n"
        "• CPU cores and memory resource limits\n"
        "• Optional network access with package manager\n"
        "• Real-time resource monitoring\n\n"
        "<b>Requirements:</b>\n"
        "• Linux with namespace support\n"
        "• Root privileges for network sandboxes\n"
        "• busybox for minimal shell"
    );
    
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

// ===== Slider/Spin Synchronization Callbacks =====

static gboolean updating_memory = FALSE;
static gboolean updating_cpu = FALSE;

static void update_memory_info_label(void) {
    if (!label_memory_info) return;
    int current = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_memory));
    char buf[128];
    snprintf(buf, sizeof(buf), "Available: %ld MB / %ld MB total", 
             g_system_available_memory_mb, g_system_total_memory_mb);
    gtk_label_set_text(GTK_LABEL(label_memory_info), buf);
}

static void update_cpu_info_label(void) {
    if (!label_cpu_info) return;
    int current = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_cpu));
    char buf[128];
    snprintf(buf, sizeof(buf), "%d / %d cores available", current, g_system_cpu_cores);
    gtk_label_set_text(GTK_LABEL(label_cpu_info), buf);
}

static void on_memory_slider_changed(GtkRange *range, gpointer user_data) {
    (void)user_data;
    if (updating_memory) return;
    updating_memory = TRUE;
    int value = (int)gtk_range_get_value(range);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_memory), value);
    update_memory_info_label();
    updating_memory = FALSE;
}

static void on_memory_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    if (updating_memory) return;
    updating_memory = TRUE;
    int value = (int)gtk_spin_button_get_value(spin);
    gtk_range_set_value(GTK_RANGE(scale_memory), value);
    update_memory_info_label();
    updating_memory = FALSE;
}

static void on_cpu_slider_changed(GtkRange *range, gpointer user_data) {
    (void)user_data;
    if (updating_cpu) return;
    updating_cpu = TRUE;
    int value = (int)gtk_range_get_value(range);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_cpu), value);
    update_cpu_info_label();
    updating_cpu = FALSE;
}

static void on_cpu_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    if (updating_cpu) return;
    updating_cpu = TRUE;
    int value = (int)gtk_spin_button_get_value(spin);
    gtk_range_set_value(GTK_RANGE(scale_cpu), value);
    update_cpu_info_label();
    updating_cpu = FALSE;
}

// ===== Quick Template Callbacks =====

static void apply_template(int memory_mb, int cores, gboolean network, const char *name_prefix) {
    // Generate unique name
    char name[64];
    time_t now = time(NULL);
    snprintf(name, sizeof(name), "%s_%ld", name_prefix, now % 10000);
    gtk_entry_set_text(GTK_ENTRY(entry_name), name);
    
    // Clamp values to system limits
    if (memory_mb > g_system_total_memory_mb * 80 / 100) {
        memory_mb = g_system_total_memory_mb * 80 / 100;
    }
    if (cores > g_system_cpu_cores) cores = g_system_cpu_cores;
    
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_memory), memory_mb);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_cpu), cores);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_network), network);
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Template applied: %d MB, %d cores, %s", 
             memory_mb, cores, network ? "network" : "isolated");
    update_status_bar(msg);
}

static void on_template_dev_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    // Development: High resources, network enabled
    int dev_memory = g_system_total_memory_mb / 2;
    if (dev_memory < 512) dev_memory = 512;
    if (dev_memory > 8192) dev_memory = 8192;
    int dev_cores = g_system_cpu_cores * 3 / 4;
    if (dev_cores < 2) dev_cores = 2;
    apply_template(dev_memory, dev_cores, TRUE, "dev");
}

static void on_template_secure_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    // Secure: Minimal resources, no network
    apply_template(256, 1, FALSE, "secure");
}

static void on_template_testing_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    // Testing: Medium resources, network enabled
    int test_memory = g_system_total_memory_mb / 4;
    if (test_memory < 256) test_memory = 256;
    if (test_memory > 2048) test_memory = 2048;
    int test_cores = g_system_cpu_cores / 2;
    if (test_cores < 1) test_cores = 1;
    apply_template(test_memory, test_cores, TRUE, "test");
}

// ==================== FILE EXPLORER IMPLEMENTATION ====================

// Get the currently selected sandbox name from combo box
static const char* get_selected_sandbox_name(GtkComboBoxText *combo) {
    return gtk_combo_box_text_get_active_text(combo);
}

// Populate sandbox combo box
static void populate_sandbox_combo(GtkComboBoxText *combo) {
    gtk_combo_box_text_remove_all(combo);
    for (GList *l = sandboxes; l; l = l->next) {
        Sandbox *s = l->data;
        gtk_combo_box_text_append_text(combo, s->name);
    }
    if (sandboxes) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    }
}

// File list columns
enum {
    FILE_COL_ICON,
    FILE_COL_NAME,
    FILE_COL_SIZE,
    FILE_COL_TYPE,
    FILE_COL_MODIFIED,
    FILE_COL_IS_DIR,
    FILE_COL_FULL_PATH,
    FILE_NUM_COLS
};

// Refresh file list by reading sandbox directory
static void refresh_file_list(const char *sandbox_name, const char *path) {
    if (!file_list_store || !sandbox_name || !path) return;
    
    gtk_list_store_clear(file_list_store);
    strncpy(current_file_path, path, PATH_MAX - 1);
    gtk_entry_set_text(GTK_ENTRY(file_path_entry), path);
    
    // Build sandbox path
    char sandbox_path[PATH_MAX];
    snprintf(sandbox_path, sizeof(sandbox_path), "/tmp/sandbox_root%s", path);
    
    DIR *dir = opendir(sandbox_path);
    if (!dir) {
        GtkTreeIter iter;
        gtk_list_store_append(file_list_store, &iter);
        gtk_list_store_set(file_list_store, &iter,
            FILE_COL_ICON, "dialog-error",
            FILE_COL_NAME, "Cannot open directory",
            FILE_COL_SIZE, "",
            FILE_COL_TYPE, "",
            FILE_COL_MODIFIED, "",
            FILE_COL_IS_DIR, FALSE,
            FILE_COL_FULL_PATH, "",
            -1);
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0) continue;
        
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", sandbox_path, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) != 0) continue;
        
        gboolean is_dir = S_ISDIR(st.st_mode);
        const char *icon = is_dir ? "folder" : "text-x-generic";
        
        // Format size
        char size_str[32] = "-";
        if (!is_dir) {
            if (st.st_size < 1024) {
                snprintf(size_str, sizeof(size_str), "%ld B", st.st_size);
            } else if (st.st_size < 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f KB", st.st_size / 1024.0);
            } else {
                snprintf(size_str, sizeof(size_str), "%.1f MB", st.st_size / (1024.0 * 1024.0));
            }
        }
        
        // Format type
        const char *type_str = is_dir ? "Folder" : "File";
        
        // Format modified time
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
        
        // Build relative path
        char rel_path[PATH_MAX];
        if (strcmp(path, "/") == 0) {
            snprintf(rel_path, sizeof(rel_path), "/%s", entry->d_name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", path, entry->d_name);
        }
        
        GtkTreeIter iter;
        gtk_list_store_append(file_list_store, &iter);
        gtk_list_store_set(file_list_store, &iter,
            FILE_COL_ICON, icon,
            FILE_COL_NAME, entry->d_name,
            FILE_COL_SIZE, size_str,
            FILE_COL_TYPE, type_str,
            FILE_COL_MODIFIED, time_str,
            FILE_COL_IS_DIR, is_dir,
            FILE_COL_FULL_PATH, rel_path,
            -1);
    }
    closedir(dir);
}

static void on_file_go_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(file_explorer_sandbox_combo));
    const char *path = gtk_entry_get_text(GTK_ENTRY(file_path_entry));
    if (sandbox && path && *path) {
        refresh_file_list(sandbox, path);
    }
}

static void on_file_up_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (strcmp(current_file_path, "/") == 0) return;
    
    char *last_slash = strrchr(current_file_path, '/');
    if (last_slash && last_slash != current_file_path) {
        *last_slash = '\0';
    } else {
        strcpy(current_file_path, "/");
    }
    
    const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(file_explorer_sandbox_combo));
    if (sandbox) {
        refresh_file_list(sandbox, current_file_path);
    }
}

static void on_file_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(file_explorer_sandbox_combo));
    if (sandbox) {
        refresh_file_list(sandbox, current_file_path);
    }
}

static void on_file_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
    (void)column;
    (void)user_data;
    
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    
    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gboolean is_dir;
        gchar *full_path;
        gtk_tree_model_get(model, &iter, FILE_COL_IS_DIR, &is_dir, FILE_COL_FULL_PATH, &full_path, -1);
        
        if (is_dir && full_path) {
            const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(file_explorer_sandbox_combo));
            if (sandbox) {
                refresh_file_list(sandbox, full_path);
            }
        }
        g_free(full_path);
    }
}

static void on_file_upload_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Select File to Upload",
        NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Upload", GTK_RESPONSE_ACCEPT, NULL);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        char *basename_str = g_path_get_basename(filename);
        
        char dest_path[PATH_MAX];
        snprintf(dest_path, sizeof(dest_path), "/tmp/sandbox_root%s/%s", current_file_path, basename_str);
        
        char cmd[PATH_MAX * 2 + 32];
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", filename, dest_path);
        
        if (system(cmd) == 0) {
            update_status_bar("File uploaded successfully");
            const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(file_explorer_sandbox_combo));
            if (sandbox) refresh_file_list(sandbox, current_file_path);
        } else {
            update_status_bar("Upload failed");
        }
        
        g_free(basename_str);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void on_file_download_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(file_tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        update_status_bar("Please select a file to download");
        return;
    }
    
    gboolean is_dir;
    gchar *full_path;
    gtk_tree_model_get(model, &iter, FILE_COL_IS_DIR, &is_dir, FILE_COL_FULL_PATH, &full_path, -1);
    
    if (is_dir) {
        update_status_bar("Cannot download directories");
        g_free(full_path);
        return;
    }
    
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Save File As",
        NULL, GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT, NULL);
    
    gchar *basename_str = g_path_get_basename(full_path);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), basename_str);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *dest = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        char src_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "/tmp/sandbox_root%s", full_path);
        
        char cmd[PATH_MAX * 2 + 32];
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", src_path, dest);
        
        if (system(cmd) == 0) {
            update_status_bar("File downloaded successfully");
        } else {
            update_status_bar("Download failed");
        }
        g_free(dest);
    }
    
    g_free(basename_str);
    g_free(full_path);
    gtk_widget_destroy(dialog);
}

static void on_file_delete_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(file_tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        update_status_bar("Please select a file or folder to delete");
        return;
    }
    
    gchar *name, *full_path;
    gboolean is_dir;
    gtk_tree_model_get(model, &iter, FILE_COL_NAME, &name, FILE_COL_FULL_PATH, &full_path, FILE_COL_IS_DIR, &is_dir, -1);
    
    // Confirm deletion
    char msg[512];
    snprintf(msg, sizeof(msg), "Delete %s '%s'?", is_dir ? "folder" : "file", name);
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, "%s", msg);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES) {
        char path_to_delete[PATH_MAX];
        snprintf(path_to_delete, sizeof(path_to_delete), "/tmp/sandbox_root%s", full_path);
        
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path_to_delete);
        
        if (system(cmd) == 0) {
            update_status_bar("Deleted successfully");
            const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(file_explorer_sandbox_combo));
            if (sandbox) refresh_file_list(sandbox, current_file_path);
        } else {
            update_status_bar("Delete failed");
        }
    }
    
    g_free(name);
    g_free(full_path);
    gtk_widget_destroy(dialog);
}

static void on_file_new_folder_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkWidget *dialog = gtk_dialog_new_with_buttons("New Folder", NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_ACCEPT, NULL);
    
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Folder name...");
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show_all(dialog);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (name && *name) {
            char new_path[PATH_MAX];
            snprintf(new_path, sizeof(new_path), "/tmp/sandbox_root%s/%s", current_file_path, name);
            
            char cmd[PATH_MAX + 32];
            snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", new_path);
            
            if (system(cmd) == 0) {
                update_status_bar("Folder created");
                const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(file_explorer_sandbox_combo));
                if (sandbox) refresh_file_list(sandbox, current_file_path);
            } else {
                update_status_bar("Failed to create folder");
            }
        }
    }
    gtk_widget_destroy(dialog);
}

// Create File Explorer tab
static GtkWidget *create_file_explorer_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 15);
    
    // Header with sandbox selector
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);
    
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>📁 File Explorer</span>");
    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);
    
    GtkWidget *sandbox_label = gtk_label_new("Sandbox:");
    gtk_box_pack_start(GTK_BOX(header), sandbox_label, FALSE, FALSE, 10);
    
    file_explorer_sandbox_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(file_explorer_sandbox_combo, 150, -1);
    gtk_box_pack_start(GTK_BOX(header), file_explorer_sandbox_combo, FALSE, FALSE, 0);
    
    // Path bar
    GtkWidget *path_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), path_bar, FALSE, FALSE, 0);
    
    GtkWidget *path_label = gtk_label_new("Path:");
    gtk_box_pack_start(GTK_BOX(path_bar), path_label, FALSE, FALSE, 0);
    
    file_path_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(file_path_entry), "/");
    gtk_box_pack_start(GTK_BOX(path_bar), file_path_entry, TRUE, TRUE, 0);
    
    GtkWidget *btn_go = gtk_button_new_with_label("→ Go");
    g_signal_connect(btn_go, "clicked", G_CALLBACK(on_file_go_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(path_bar), btn_go, FALSE, FALSE, 0);
    
    GtkWidget *btn_up = gtk_button_new_with_label("↑ Up");
    g_signal_connect(btn_up, "clicked", G_CALLBACK(on_file_up_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(path_bar), btn_up, FALSE, FALSE, 0);
    
    GtkWidget *btn_refresh = gtk_button_new_with_label("↻");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_file_refresh_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(path_bar), btn_refresh, FALSE, FALSE, 0);
    
    // File list
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);
    
    file_list_store = gtk_list_store_new(FILE_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);
    file_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(file_list_store));
    g_object_unref(file_list_store);
    
    // Icon column
    GtkCellRenderer *icon_renderer = gtk_cell_renderer_pixbuf_new();
    GtkTreeViewColumn *icon_col = gtk_tree_view_column_new_with_attributes("", icon_renderer, "icon-name", FILE_COL_ICON, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(file_tree_view), icon_col);
    
    // Name column
    GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes("Name", text_renderer, "text", FILE_COL_NAME, NULL);
    gtk_tree_view_column_set_expand(name_col, TRUE);
    gtk_tree_view_column_set_min_width(name_col, 200);
    gtk_tree_view_append_column(GTK_TREE_VIEW(file_tree_view), name_col);
    
    // Size column
    GtkTreeViewColumn *size_col = gtk_tree_view_column_new_with_attributes("Size", text_renderer, "text", FILE_COL_SIZE, NULL);
    gtk_tree_view_column_set_min_width(size_col, 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(file_tree_view), size_col);
    
    // Type column
    GtkTreeViewColumn *type_col = gtk_tree_view_column_new_with_attributes("Type", text_renderer, "text", FILE_COL_TYPE, NULL);
    gtk_tree_view_column_set_min_width(type_col, 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(file_tree_view), type_col);
    
    // Modified column
    GtkTreeViewColumn *mod_col = gtk_tree_view_column_new_with_attributes("Modified", text_renderer, "text", FILE_COL_MODIFIED, NULL);
    gtk_tree_view_column_set_min_width(mod_col, 140);
    gtk_tree_view_append_column(GTK_TREE_VIEW(file_tree_view), mod_col);
    
    g_signal_connect(file_tree_view, "row-activated", G_CALLBACK(on_file_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), file_tree_view);
    
    // Action buttons
    GtkWidget *action_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), action_bar, FALSE, FALSE, 0);
    
    GtkWidget *btn_upload = gtk_button_new_with_label("📤 Upload");
    g_signal_connect(btn_upload, "clicked", G_CALLBACK(on_file_upload_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), btn_upload, FALSE, FALSE, 0);
    
    GtkWidget *btn_download = gtk_button_new_with_label("📥 Download");
    g_signal_connect(btn_download, "clicked", G_CALLBACK(on_file_download_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), btn_download, FALSE, FALSE, 0);
    
    GtkWidget *btn_new_folder = gtk_button_new_with_label("📁 New Folder");
    g_signal_connect(btn_new_folder, "clicked", G_CALLBACK(on_file_new_folder_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), btn_new_folder, FALSE, FALSE, 0);
    
    GtkWidget *btn_delete = gtk_button_new_with_label("🗑 Delete");
    GtkStyleContext *ctx = gtk_widget_get_style_context(btn_delete);
    gtk_style_context_add_class(ctx, "danger-button");
    g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_file_delete_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), btn_delete, FALSE, FALSE, 0);
    
    return vbox;
}

// ==================== PROCESS MANAGER IMPLEMENTATION ====================

// Process list columns
enum {
    PROC_COL_PID,
    PROC_COL_NAME,
    PROC_COL_CPU,
    PROC_COL_MEM,
    PROC_COL_STATE,
    PROC_COL_COMMAND,
    PROC_NUM_COLS
};

// Refresh process list
static void refresh_process_list(const char *sandbox_name) {
    if (!process_list_store || !sandbox_name) return;
    
    gtk_list_store_clear(process_list_store);
    
    // Read processes from /proc inside sandbox
    // For now, we'll read from host /proc (sandbox processes visible)
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) return;
    
    struct dirent *entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        // Only process numeric directories (PIDs)
        char *endptr;
        long pid = strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid <= 0) continue;
        
        // Read /proc/[pid]/stat
        char stat_path[PATH_MAX];
        snprintf(stat_path, sizeof(stat_path), "/proc/%ld/stat", pid);
        
        FILE *f = fopen(stat_path, "r");
        if (!f) continue;
        
        char comm[256] = "";
        char state = '?';
        unsigned long utime = 0, stime = 0;
        long rss = 0;
        
        // Parse stat file
        int scanned = fscanf(f, "%*d (%255[^)]) %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
            comm, &state, &utime, &stime, &rss);
        fclose(f);
        
        if (scanned < 3) continue;
        
        // Skip kernel threads and system processes for cleaner view
        if (pid < 100) continue;
        
        // Format CPU (simplified - just show percentage of total time)
        double cpu_percent = (utime + stime) / 100.0;
        char cpu_str[32];
        snprintf(cpu_str, sizeof(cpu_str), "%.1f%%", cpu_percent > 100 ? 100 : cpu_percent);
        
        // Format memory
        char mem_str[32];
        long mem_kb = rss * 4; // Assuming 4KB page size
        if (mem_kb < 1024) {
            snprintf(mem_str, sizeof(mem_str), "%ld KB", mem_kb);
        } else {
            snprintf(mem_str, sizeof(mem_str), "%.1f MB", mem_kb / 1024.0);
        }
        
        // Format state
        const char *state_str;
        switch (state) {
            case 'R': state_str = "Running"; break;
            case 'S': state_str = "Sleeping"; break;
            case 'D': state_str = "Disk I/O"; break;
            case 'Z': state_str = "Zombie"; break;
            case 'T': state_str = "Stopped"; break;
            default: state_str = "Unknown"; break;
        }
        
        // Read cmdline
        char cmdline_path[PATH_MAX];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%ld/cmdline", pid);
        char cmdline[256] = "";
        FILE *cmd_f = fopen(cmdline_path, "r");
        if (cmd_f) {
            size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, cmd_f);
            if (n > 0) {
                // Replace null bytes with spaces
                for (size_t i = 0; i < n; i++) {
                    if (cmdline[i] == '\0') cmdline[i] = ' ';
                }
                cmdline[n] = '\0';
            }
            fclose(cmd_f);
        }
        if (cmdline[0] == '\0') {
            snprintf(cmdline, sizeof(cmdline), "[%s]", comm);
        }
        
        GtkTreeIter iter;
        gtk_list_store_append(process_list_store, &iter);
        gtk_list_store_set(process_list_store, &iter,
            PROC_COL_PID, (int)pid,
            PROC_COL_NAME, comm,
            PROC_COL_CPU, cpu_str,
            PROC_COL_MEM, mem_str,
            PROC_COL_STATE, state_str,
            PROC_COL_COMMAND, cmdline,
            -1);
    }
    closedir(proc_dir);
}

static void on_process_kill_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(process_tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        update_status_bar("Please select a process to kill");
        return;
    }
    
    int pid;
    gchar *name;
    gtk_tree_model_get(model, &iter, PROC_COL_PID, &pid, PROC_COL_NAME, &name, -1);
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Kill process %d (%s)?", pid, name);
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, "%s", msg);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "kill -9 %d 2>/dev/null", pid);
        system(cmd);
        update_status_bar("Process killed");
        
        const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(process_sandbox_combo));
        if (sandbox) refresh_process_list(sandbox);
    }
    
    g_free(name);
    gtk_widget_destroy(dialog);
}

static void on_process_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(process_sandbox_combo));
    if (sandbox) {
        refresh_process_list(sandbox);
        update_status_bar("Process list refreshed");
    }
}

static gboolean on_process_auto_refresh(gpointer user_data) {
    (void)user_data;
    const char *sandbox = get_selected_sandbox_name(GTK_COMBO_BOX_TEXT(process_sandbox_combo));
    if (sandbox) refresh_process_list(sandbox);
    return TRUE; // Continue timer
}

static void on_process_auto_toggle(GtkToggleButton *button, gpointer user_data) {
    (void)user_data;
    if (gtk_toggle_button_get_active(button)) {
        if (process_refresh_timer == 0) {
            process_refresh_timer = g_timeout_add(2000, on_process_auto_refresh, NULL);
            update_status_bar("Auto-refresh enabled (2s)");
        }
    } else {
        if (process_refresh_timer > 0) {
            g_source_remove(process_refresh_timer);
            process_refresh_timer = 0;
            update_status_bar("Auto-refresh disabled");
        }
    }
}

// Create Process Manager tab
static GtkWidget *create_process_manager_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 15);
    
    // Header
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);
    
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>⚡ Process Manager</span>");
    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);
    
    GtkWidget *sandbox_label = gtk_label_new("Sandbox:");
    gtk_box_pack_start(GTK_BOX(header), sandbox_label, FALSE, FALSE, 10);
    
    process_sandbox_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(process_sandbox_combo, 150, -1);
    gtk_box_pack_start(GTK_BOX(header), process_sandbox_combo, FALSE, FALSE, 0);
    
    // Process list
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);
    
    process_list_store = gtk_list_store_new(PROC_NUM_COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    process_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(process_list_store));
    g_object_unref(process_list_store);
    
    GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
    
    // PID column
    GtkTreeViewColumn *pid_col = gtk_tree_view_column_new_with_attributes("PID", text_renderer, "text", PROC_COL_PID, NULL);
    gtk_tree_view_column_set_min_width(pid_col, 60);
    gtk_tree_view_append_column(GTK_TREE_VIEW(process_tree_view), pid_col);
    
    // Name column
    GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes("Name", text_renderer, "text", PROC_COL_NAME, NULL);
    gtk_tree_view_column_set_min_width(name_col, 120);
    gtk_tree_view_append_column(GTK_TREE_VIEW(process_tree_view), name_col);
    
    // CPU column
    GtkTreeViewColumn *cpu_col = gtk_tree_view_column_new_with_attributes("CPU", text_renderer, "text", PROC_COL_CPU, NULL);
    gtk_tree_view_column_set_min_width(cpu_col, 70);
    gtk_tree_view_append_column(GTK_TREE_VIEW(process_tree_view), cpu_col);
    
    // Memory column
    GtkTreeViewColumn *mem_col = gtk_tree_view_column_new_with_attributes("Memory", text_renderer, "text", PROC_COL_MEM, NULL);
    gtk_tree_view_column_set_min_width(mem_col, 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(process_tree_view), mem_col);
    
    // State column
    GtkTreeViewColumn *state_col = gtk_tree_view_column_new_with_attributes("State", text_renderer, "text", PROC_COL_STATE, NULL);
    gtk_tree_view_column_set_min_width(state_col, 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(process_tree_view), state_col);
    
    // Command column
    GtkTreeViewColumn *cmd_col = gtk_tree_view_column_new_with_attributes("Command", text_renderer, "text", PROC_COL_COMMAND, NULL);
    gtk_tree_view_column_set_expand(cmd_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(process_tree_view), cmd_col);
    
    gtk_container_add(GTK_CONTAINER(scrolled), process_tree_view);
    
    // Action buttons
    GtkWidget *action_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), action_bar, FALSE, FALSE, 0);
    
    GtkWidget *btn_kill = gtk_button_new_with_label("🔪 Kill Process");
    GtkStyleContext *ctx = gtk_widget_get_style_context(btn_kill);
    gtk_style_context_add_class(ctx, "danger-button");
    g_signal_connect(btn_kill, "clicked", G_CALLBACK(on_process_kill_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), btn_kill, FALSE, FALSE, 0);
    
    GtkWidget *btn_refresh = gtk_button_new_with_label("↻ Refresh");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_process_refresh_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), btn_refresh, FALSE, FALSE, 0);
    
    process_auto_refresh_check = gtk_check_button_new_with_label("Auto-refresh (2s)");
    g_signal_connect(process_auto_refresh_check, "toggled", G_CALLBACK(on_process_auto_toggle), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), process_auto_refresh_check, FALSE, FALSE, 10);
    
    return vbox;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    // Detect system resources first
    detect_system_resources();
    
    // Initialize paths based on executable location
    init_paths(argv[0]);
    
    // Apply CSS styling
    apply_css_styling();
    
    load_sandboxes();

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Linux Sandbox Manager");
    gtk_window_set_default_size(GTK_WINDOW(window), 1024, 700);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    log_buffer = g_queue_new();

    // Main vertical box for entire window
    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);
    
    // ===== HEADER BAR =====
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(header_box, 10);
    gtk_widget_set_margin_end(header_box, 10);
    gtk_widget_set_margin_top(header_box, 10);
    gtk_widget_set_margin_bottom(header_box, 5);
    gtk_box_pack_start(GTK_BOX(main_vbox), header_box, FALSE, FALSE, 0);
    
    GtkWidget *title_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title_label), "<span size='x-large' weight='bold' color='#00d9ff'>🔒 Linux Sandbox Manager</span>");
    GtkStyleContext *title_ctx = gtk_widget_get_style_context(title_label);
    gtk_style_context_add_class(title_ctx, "app-title");
    gtk_box_pack_start(GTK_BOX(header_box), title_label, FALSE, FALSE, 0);
    
    // Spacer
    GtkWidget *spacer = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(header_box), spacer, TRUE, TRUE, 0);
    
    // About button
    GtkWidget *btn_about = gtk_button_new_with_label("ℹ About");
    g_signal_connect(btn_about, "clicked", G_CALLBACK(on_about_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(header_box), btn_about, FALSE, FALSE, 0);
    
    // Separator
    gtk_box_pack_start(GTK_BOX(main_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    // ===== SYSTEM INFO BAR =====
    GtkWidget *sysinfo_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_margin_start(sysinfo_box, 10);
    gtk_widget_set_margin_end(sysinfo_box, 10);
    gtk_widget_set_margin_top(sysinfo_box, 8);
    gtk_widget_set_margin_bottom(sysinfo_box, 8);
    gtk_box_pack_start(GTK_BOX(main_vbox), sysinfo_box, FALSE, FALSE, 0);
    
    // CPU Bar
    GtkWidget *cpu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *cpu_label = gtk_label_new("System CPU");
    gtk_widget_set_halign(cpu_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(cpu_box), cpu_label, FALSE, FALSE, 0);
    sys_cpu_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(sys_cpu_bar), "CPU: 0%");
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(sys_cpu_bar), TRUE);
    gtk_widget_set_size_request(sys_cpu_bar, 180, -1);
    gtk_box_pack_start(GTK_BOX(cpu_box), sys_cpu_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sysinfo_box), cpu_box, FALSE, FALSE, 0);
    
    // Memory Bar
    GtkWidget *mem_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *mem_label = gtk_label_new("System Memory");
    gtk_widget_set_halign(mem_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(mem_box), mem_label, FALSE, FALSE, 0);
    sys_mem_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(sys_mem_bar), "Mem: 0 MB");
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(sys_mem_bar), TRUE);
    gtk_widget_set_size_request(sys_mem_bar, 220, -1);
    gtk_box_pack_start(GTK_BOX(mem_box), sys_mem_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sysinfo_box), mem_box, FALSE, FALSE, 0);
    
    // Uptime
    sys_uptime_label = gtk_label_new("Uptime: --");
    gtk_box_pack_start(GTK_BOX(sysinfo_box), sys_uptime_label, FALSE, FALSE, 0);
    
    // Sandbox count
    sandbox_count_label = gtk_label_new("Sandboxes: 0");
    gtk_box_pack_start(GTK_BOX(sysinfo_box), sandbox_count_label, FALSE, FALSE, 0);
    
    // Separator
    gtk_box_pack_start(GTK_BOX(main_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    // ===== NOTEBOOK (Main Content) =====
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(main_vbox), notebook, TRUE, TRUE, 0);

    // ===== SANDBOXES TAB =====
    GtkWidget *manager_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), manager_paned, gtk_label_new("📦 Sandboxes"));
    
    // Left side - Create form and list
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(left_box, 10);
    gtk_widget_set_margin_end(left_box, 5);
    gtk_widget_set_margin_top(left_box, 10);
    gtk_widget_set_margin_bottom(left_box, 10);
    gtk_paned_pack1(GTK_PANED(manager_paned), left_box, TRUE, FALSE);
    
    // Create sandbox frame
    GtkWidget *create_frame = gtk_frame_new("Create New Sandbox");
    gtk_box_pack_start(GTK_BOX(left_box), create_frame, FALSE, FALSE, 0);
    
    GtkWidget *create_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(create_box, 10);
    gtk_widget_set_margin_end(create_box, 10);
    gtk_widget_set_margin_top(create_box, 10);
    gtk_widget_set_margin_bottom(create_box, 10);
    gtk_container_add(GTK_CONTAINER(create_frame), create_box);

    // Name row
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(create_box), hbox, FALSE, FALSE, 0);
    GtkWidget *label = gtk_label_new("Name:");
    gtk_widget_set_size_request(label, 100, -1);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    entry_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_name), "Enter sandbox name...");
    gtk_box_pack_start(GTK_BOX(hbox), entry_name, TRUE, TRUE, 0);

    // Memory row with slider + spin button
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(create_box), hbox, FALSE, FALSE, 0);
    label = gtk_label_new("Memory (MB):");
    gtk_widget_set_size_request(label, 100, -1);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    
    // Dynamic memory slider: 64 MB to 80% of system memory
    long max_memory = g_system_total_memory_mb * 80 / 100;
    if (max_memory < 256) max_memory = 256;
    scale_memory = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 64, max_memory, 64);
    int default_memory = g_system_total_memory_mb / 4;
    if (default_memory < 256) default_memory = 256;
    if (default_memory > 4096) default_memory = 4096;
    gtk_range_set_value(GTK_RANGE(scale_memory), default_memory);
    gtk_scale_set_draw_value(GTK_SCALE(scale_memory), FALSE);
    gtk_widget_set_hexpand(scale_memory, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), scale_memory, TRUE, TRUE, 0);
    
    // Spin button for manual entry
    spin_memory = gtk_spin_button_new_with_range(64, max_memory, 64);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_memory), default_memory);
    gtk_widget_set_size_request(spin_memory, 80, -1);
    gtk_box_pack_start(GTK_BOX(hbox), spin_memory, FALSE, FALSE, 0);
    
    // Connect synchronization signals
    g_signal_connect(scale_memory, "value-changed", G_CALLBACK(on_memory_slider_changed), NULL);
    g_signal_connect(spin_memory, "value-changed", G_CALLBACK(on_memory_spin_changed), NULL);
    
    // Memory info label
    label_memory_info = gtk_label_new("");
    gtk_widget_set_halign(label_memory_info, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(create_box), label_memory_info, FALSE, FALSE, 0);
    update_memory_info_label();

    // CPU cores row with slider + spin button
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(create_box), hbox, FALSE, FALSE, 0);
    label = gtk_label_new("CPU Cores:");
    gtk_widget_set_size_request(label, 100, -1);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    
    // Dynamic CPU slider: 1 to system cores
    scale_cpu = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, g_system_cpu_cores, 1);
    int default_cores = g_system_cpu_cores / 2;
    if (default_cores < 1) default_cores = 1;
    gtk_range_set_value(GTK_RANGE(scale_cpu), default_cores);
    gtk_scale_set_draw_value(GTK_SCALE(scale_cpu), FALSE);
    gtk_widget_set_hexpand(scale_cpu, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), scale_cpu, TRUE, TRUE, 0);
    
    // Spin button for manual entry
    spin_cpu = gtk_spin_button_new_with_range(1, g_system_cpu_cores, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_cpu), default_cores);
    gtk_widget_set_size_request(spin_cpu, 60, -1);
    gtk_box_pack_start(GTK_BOX(hbox), spin_cpu, FALSE, FALSE, 0);
    
    // Connect synchronization signals
    g_signal_connect(scale_cpu, "value-changed", G_CALLBACK(on_cpu_slider_changed), NULL);
    g_signal_connect(spin_cpu, "value-changed", G_CALLBACK(on_cpu_spin_changed), NULL);
    
    // CPU info label
    label_cpu_info = gtk_label_new("");
    gtk_widget_set_halign(label_cpu_info, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(create_box), label_cpu_info, FALSE, FALSE, 0);
    update_cpu_info_label();

    // Network checkbox
    check_network = gtk_check_button_new_with_label("🌐 Enable Network Access (requires root, enables apt)");
    gtk_box_pack_start(GTK_BOX(create_box), check_network, FALSE, FALSE, 0);

    // Quick template buttons
    GtkWidget *template_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(create_box), template_box, FALSE, FALSE, 0);
    
    GtkWidget *template_label = gtk_label_new("Templates:");
    gtk_box_pack_start(GTK_BOX(template_box), template_label, FALSE, FALSE, 0);
    
    GtkWidget *btn_template_dev = gtk_button_new_with_label("🛠 Dev");
    g_signal_connect(btn_template_dev, "clicked", G_CALLBACK(on_template_dev_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(template_box), btn_template_dev, FALSE, FALSE, 0);
    
    GtkWidget *btn_template_secure = gtk_button_new_with_label("🔒 Secure");
    g_signal_connect(btn_template_secure, "clicked", G_CALLBACK(on_template_secure_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(template_box), btn_template_secure, FALSE, FALSE, 0);
    
    GtkWidget *btn_template_test = gtk_button_new_with_label("🧪 Test");
    g_signal_connect(btn_template_test, "clicked", G_CALLBACK(on_template_testing_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(template_box), btn_template_test, FALSE, FALSE, 0);

    // Create buttons
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(create_box), hbox, FALSE, FALSE, 0);

    GtkWidget *btn_create = gtk_button_new_with_label("➕ Create Sandbox");
    g_signal_connect(btn_create, "clicked", G_CALLBACK(on_create_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), btn_create, TRUE, TRUE, 0);

    GtkWidget *btn_clear = gtk_button_new_with_label("🔄 Clear Form");
    g_signal_connect(btn_clear, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), btn_clear, TRUE, TRUE, 0);

    // Sandbox list frame
    GtkWidget *list_frame = gtk_frame_new("Existing Sandboxes");
    gtk_box_pack_start(GTK_BOX(left_box), list_frame, TRUE, TRUE, 0);
    
    GtkWidget *list_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(list_vbox, 5);
    gtk_widget_set_margin_end(list_vbox, 5);
    gtk_widget_set_margin_top(list_vbox, 5);
    gtk_widget_set_margin_bottom(list_vbox, 5);
    gtk_container_add(GTK_CONTAINER(list_frame), list_vbox);
    
    // Action buttons for list
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(list_vbox), action_box, FALSE, FALSE, 0);
    
    GtkWidget *btn_enter = gtk_button_new_with_label("▶ Enter");
    g_signal_connect(btn_enter, "clicked", G_CALLBACK(on_enter_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_box), btn_enter, TRUE, TRUE, 0);

    GtkWidget *btn_delete = gtk_button_new_with_label("🗑 Delete");
    g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_delete_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_box), btn_delete, TRUE, TRUE, 0);
    
    GtkWidget *btn_refresh = gtk_button_new_with_label("↻ Refresh");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_box), btn_refresh, TRUE, TRUE, 0);

    // List
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(list_vbox), scrolled, TRUE, TRUE, 0);

    listbox = gtk_list_box_new();
    g_signal_connect(listbox, "row-selected", G_CALLBACK(on_listbox_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);
    
    // Right side - Sandbox details panel
    detail_panel = gtk_frame_new("Sandbox Details");
    gtk_widget_set_size_request(detail_panel, 280, -1);
    gtk_widget_set_margin_start(detail_panel, 5);
    gtk_widget_set_margin_end(detail_panel, 10);
    gtk_widget_set_margin_top(detail_panel, 10);
    gtk_widget_set_margin_bottom(detail_panel, 10);
    gtk_paned_pack2(GTK_PANED(manager_paned), detail_panel, FALSE, FALSE);
    
    GtkWidget *detail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(detail_box, 15);
    gtk_widget_set_margin_end(detail_box, 15);
    gtk_widget_set_margin_top(detail_box, 15);
    gtk_widget_set_margin_bottom(detail_box, 15);
    gtk_container_add(GTK_CONTAINER(detail_panel), detail_box);
    
    detail_name_label = gtk_label_new("");
    gtk_widget_set_halign(detail_name_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(detail_box), detail_name_label, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(detail_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 5);
    
    detail_memory_label = gtk_label_new("");
    gtk_widget_set_halign(detail_memory_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(detail_box), detail_memory_label, FALSE, FALSE, 0);
    
    detail_cpu_label = gtk_label_new("");
    gtk_widget_set_halign(detail_cpu_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(detail_box), detail_cpu_label, FALSE, FALSE, 0);
    
    detail_network_label = gtk_label_new("");
    gtk_widget_set_halign(detail_network_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(detail_box), detail_network_label, FALSE, FALSE, 0);
    
    detail_created_label = gtk_label_new("");
    gtk_widget_set_halign(detail_created_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(detail_box), detail_created_label, FALSE, FALSE, 0);
    
    // Help text in detail panel
    GtkWidget *help_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(help_label), 
        "\n<span size='small' color='#666666'>"
        "<b>Tips:</b>\n"
        "• Network-enabled sandboxes need root\n"
        "• Isolated sandboxes are more secure\n"
        "• Use Enter to access the shell\n"
        "</span>");
    gtk_label_set_line_wrap(GTK_LABEL(help_label), TRUE);
    gtk_widget_set_halign(help_label, GTK_ALIGN_START);
    gtk_box_pack_end(GTK_BOX(detail_box), help_label, FALSE, FALSE, 0);
    
    gtk_paned_set_position(GTK_PANED(manager_paned), 650);

    // ===== LOGS TAB =====
    GtkWidget *logs_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(logs_box, 10);
    gtk_widget_set_margin_end(logs_box, 10);
    gtk_widget_set_margin_top(logs_box, 10);
    gtk_widget_set_margin_bottom(logs_box, 10);
    
    // Log toolbar
    GtkWidget *log_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(logs_box), log_toolbar, FALSE, FALSE, 0);
    
    GtkWidget *btn_export = gtk_button_new_with_label("📥 Export Logs");
    g_signal_connect(btn_export, "clicked", G_CALLBACK(on_export_logs_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(log_toolbar), btn_export, FALSE, FALSE, 0);
    
    GtkWidget *log_hint = gtk_label_new(NULL);
    char *log_hint_markup = g_strdup_printf("<span color='#666666'>Logs are also written to %s</span>", LOG_FILE);
    gtk_label_set_markup(GTK_LABEL(log_hint), log_hint_markup);
    g_free(log_hint_markup);
    gtk_box_pack_end(GTK_BOX(log_toolbar), log_hint, FALSE, FALSE, 0);
    
    GtkWidget *log_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view), TRUE);
    gtk_container_add(GTK_CONTAINER(log_scrolled), log_view);
    gtk_box_pack_start(GTK_BOX(logs_box), log_scrolled, TRUE, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), logs_box, gtk_label_new("📋 Logs"));

    // ===== FILE EXPLORER TAB =====
    GtkWidget *file_explorer = create_file_explorer_tab();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), file_explorer, gtk_label_new("📁 Files"));
    
    // ===== PROCESS MANAGER TAB =====
    GtkWidget *process_manager = create_process_manager_tab();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), process_manager, gtk_label_new("⚡ Processes"));

    // ===== STATUS BAR =====
    status_bar = gtk_label_new("Ready");
    gtk_widget_set_halign(status_bar, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status_bar, 10);
    gtk_widget_set_margin_end(status_bar, 10);
    gtk_widget_set_margin_top(status_bar, 4);
    gtk_widget_set_margin_bottom(status_bar, 4);
    GtkStyleContext *ctx = gtk_widget_get_style_context(status_bar);
    gtk_style_context_add_class(ctx, "status-bar");
    gtk_box_pack_start(GTK_BOX(main_vbox), status_bar, FALSE, FALSE, 0);

    // Initialize displays
    update_list();
    update_log_view();
    update_sandbox_details(NULL);
    refresh_system_info_cb(NULL); // Initial update
    
    // Populate sandbox combos for File Explorer and Process Manager
    populate_sandbox_combo(GTK_COMBO_BOX_TEXT(file_explorer_sandbox_combo));
    populate_sandbox_combo(GTK_COMBO_BOX_TEXT(process_sandbox_combo));
    
    // Set up timers
    g_timeout_add_seconds(2, refresh_usage_cb, NULL);
    g_timeout_add_seconds(1, refresh_system_info_cb, NULL);
    
    log_gui_event("INFO", NULL, "Sandbox Manager started");
    update_status_bar("Ready - Select a sandbox or create a new one");

    gtk_widget_show_all(window);
    gtk_widget_hide(detail_panel); // Hide until a sandbox is selected
    gtk_main();

    // Free sandbox list
    for (GList *l = sandboxes; l; l = l->next) {
        free(l->data);
    }
    g_list_free(sandboxes);

    // Free log buffer
    if (log_buffer) {
        gchar *line;
        while ((line = g_queue_pop_head(log_buffer)) != NULL) {
            g_free(line);
        }
        g_queue_free(log_buffer);
        log_buffer = NULL;
    }

    return 0;
}