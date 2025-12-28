#include <gtk/gtk.h>
#include <vte/vte.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <linux/limits.h>

// Global paths - will be set at runtime based on executable location
static char g_config_file[PATH_MAX];
static char g_sandbox_bin[PATH_MAX];
static char g_log_file[PATH_MAX];

// Macros for compatibility with existing code
#define CONFIG_FILE g_config_file
#define SANDBOX_BIN g_sandbox_bin
#define LOG_FILE g_log_file

typedef struct {
    char name[256];
    int memory;
    int cpu;
    int network;
    time_t date;
} Sandbox;

GtkWidget *entry_name;
GtkWidget *scale_memory;
GtkWidget *scale_cpu;
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

void load_sandboxes() {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        Sandbox *s = malloc(sizeof(Sandbox));
        sscanf(line, "%s %d %d %d %ld", s->name, &s->memory, &s->cpu, &s->network, &s->date);
        sandboxes = g_list_append(sandboxes, s);
    }
    fclose(f);
}

void save_sandboxes() {
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    for (GList *l = sandboxes; l; l = l->next) {
        Sandbox *s = l->data;
        fprintf(f, "%s %d %d %d %ld\n", s->name, s->memory, s->cpu, s->network, s->date);
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
                               argv,
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
                             (char *const *)argv,
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
    int memory = (int)gtk_range_get_value(GTK_RANGE(scale_memory));
    int cpu = (int)gtk_range_get_value(GTK_RANGE(scale_cpu));
    int network = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_network));

    if (!name || !*name) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Please fill all fields");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (memory < 0 || memory > 1024 || cpu < 1 || cpu > 100) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Invalid slider values");
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

    // Build argv dynamically to avoid empty arguments
    char *argv_cmd[10] = {0};
    int idx = 0;
    argv_cmd[idx++] = SANDBOX_BIN;
    argv_cmd[idx++] = "-c";
    argv_cmd[idx++] = "-m";
    char mem_buf[16];
    snprintf(mem_buf, sizeof(mem_buf), "%d", memory);
    argv_cmd[idx++] = mem_buf;
    argv_cmd[idx++] = "-t";
    char cpu_buf[16];
    snprintf(cpu_buf, sizeof(cpu_buf), "%d", cpu);
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
    s->cpu = cpu;
    s->network = network;
    s->date = time(NULL);
    sandboxes = g_list_append(sandboxes, s);
    save_sandboxes();
    update_list();
    log_gui_event("INFO", name, "Created sandbox");
}

void on_child_exited(VteTerminal *terminal, int status, gpointer user_data) {
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
    gtk_window_set_title(GTK_WINDOW(window), "Sandbox Terminal");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), box);

    // Create terminal and pack it before interacting with it
    GtkWidget *terminal = vte_terminal_new();
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
    gtk_range_set_value(GTK_RANGE(scale_memory), 100);
    gtk_range_set_value(GTK_RANGE(scale_cpu), 10);
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
    char resolved_parent[PATH_MAX];
    if (realpath(parent_dir, resolved_parent) != NULL) {
        snprintf(g_config_file, sizeof(g_config_file), "%s/sandboxes.txt", resolved_parent);
        snprintf(g_log_file, sizeof(g_log_file), "%s/gui.log", resolved_parent);
    } else {
        snprintf(g_config_file, sizeof(g_config_file), "%s/../sandboxes.txt", dir);
        snprintf(g_log_file, sizeof(g_log_file), "%s/../gui.log", dir);
    }
}

// Apply CSS styling - Clean Light Theme
static void apply_css_styling(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css = 
        /* Main window */
        "window { background-color: #f5f5f5; }"
        
        /* Frames */
        "frame { border: 1px solid #ddd; }"
        "frame > label { font-weight: bold; color: #333; }"
        
        /* Labels */
        "label { color: #333; }"
        
        /* Buttons */
        "button { "
        "    background-image: linear-gradient(to bottom, #ffffff, #f0f0f0); "
        "    border: 1px solid #ccc; "
        "    border-radius: 4px; "
        "    padding: 6px 12px; "
        "    color: #333; "
        "}"
        "button:hover { "
        "    background-image: linear-gradient(to bottom, #e8f4fc, #d0e8f8); "
        "    border-color: #2196F3; "
        "}"
        "button:active { "
        "    background-image: linear-gradient(to bottom, #d0e8f8, #b8daf0); "
        "}"
        
        /* Entry fields */
        "entry { "
        "    background-color: white; "
        "    border: 1px solid #ccc; "
        "    border-radius: 4px; "
        "    padding: 6px; "
        "    color: #333; "
        "}"
        "entry:focus { border-color: #2196F3; }"
        
        /* Progress bars */
        "progressbar trough { "
        "    background-color: #e0e0e0; "
        "    border-radius: 4px; "
        "}"
        "progressbar progress { "
        "    background-color: #2196F3; "
        "    border-radius: 4px; "
        "}"
        
        /* Scales/Sliders */
        "scale trough { background-color: #e0e0e0; border-radius: 4px; }"
        "scale highlight { background-color: #2196F3; border-radius: 4px; }"
        "scale slider { background-color: #2196F3; border-radius: 50%; }"
        
        /* Notebook tabs */
        "notebook tab { "
        "    background-color: #e8e8e8; "
        "    padding: 8px 16px; "
        "    border: 1px solid #ccc; "
        "}"
        "notebook tab:checked { "
        "    background-color: #ffffff; "
        "    border-bottom-color: #ffffff; "
        "}"
        "notebook header { background-color: #f0f0f0; }"
        
        /* List box */
        "list { background-color: white; }"
        "list row { padding: 8px; border-bottom: 1px solid #eee; }"
        "list row:selected { background-color: #e3f2fd; }"
        
        /* Scrolled window */
        "scrolledwindow { background-color: white; border: 1px solid #ddd; }"
        
        /* Check buttons */
        "checkbutton { color: #333; }"
        
        /* Separator */
        "separator { background-color: #ddd; }"
        
        /* Status bar */
        ".status-bar { "
        "    background-color: #e8e8e8; "
        "    color: #666; "
        "    padding: 4px 8px; "
        "    font-size: 11px; "
        "    border-top: 1px solid #ccc; "
        "}"
        
        /* Text view (logs) */
        "textview { background-color: white; color: #333; }"
        "textview text { background-color: white; }";
    
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
    
    char buf[256];
    snprintf(buf, sizeof(buf), "<b>%s</b>", s->name);
    gtk_label_set_markup(GTK_LABEL(detail_name_label), buf);
    
    snprintf(buf, sizeof(buf), "Memory Limit: %d MB", s->memory);
    gtk_label_set_text(GTK_LABEL(detail_memory_label), buf);
    
    snprintf(buf, sizeof(buf), "CPU Time: %d seconds", s->cpu);
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
    
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "Linux Sandbox Manager v1.0"
    );
    
    gtk_message_dialog_format_secondary_markup(
        GTK_MESSAGE_DIALOG(dialog),
        "<b>Features:</b>\n"
        "• Create isolated sandbox environments\n"
        "• PID, User, Network namespace isolation\n"
        "• Memory and CPU resource limits\n"
        "• Optional network access\n"
        "• Real-time resource monitoring\n\n"
        "<b>Requirements:</b>\n"
        "• Linux with namespace support\n"
        "• Root privileges for some operations\n"
        "• busybox for minimal shell"
    );
    
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

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
    gtk_label_set_markup(GTK_LABEL(title_label), "<span size='x-large' weight='bold' color='#1565c0'>🔒 Linux Sandbox Manager</span>");
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

    // Memory row
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(create_box), hbox, FALSE, FALSE, 0);
    label = gtk_label_new("Memory (MB):");
    gtk_widget_set_size_request(label, 100, -1);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    scale_memory = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 1024, 1);
    gtk_range_set_value(GTK_RANGE(scale_memory), 100);
    gtk_scale_set_draw_value(GTK_SCALE(scale_memory), TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), scale_memory, TRUE, TRUE, 0);

    // CPU row
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(create_box), hbox, FALSE, FALSE, 0);
    label = gtk_label_new("CPU Time (s):");
    gtk_widget_set_size_request(label, 100, -1);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    scale_cpu = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 300, 1);
    gtk_range_set_value(GTK_RANGE(scale_cpu), 30);
    gtk_scale_set_draw_value(GTK_SCALE(scale_cpu), TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), scale_cpu, TRUE, TRUE, 0);

    // Network checkbox
    check_network = gtk_check_button_new_with_label("🌐 Enable Network Access (requires root)");
    gtk_box_pack_start(GTK_BOX(create_box), check_network, FALSE, FALSE, 0);

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
    gtk_label_set_markup(GTK_LABEL(log_hint), "<span color='#666666'>Logs are also written to " LOG_FILE "</span>");
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