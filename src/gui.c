#include <gtk/gtk.h>
#include <vte/vte.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_FILE "/home/ubuntu/sandbox/sandboxes.txt"
#define SANDBOX_BIN "/home/ubuntu/sandbox/bin/sandbox"
#define LOG_FILE "/home/ubuntu/sandbox/gui.log"

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
    // Fallback: any process line that contains the name
    if (!found) {
        rewind(fp);
        if (fgets(line, sizeof(line), fp)) { /* skip header */ }
        while (fgets(line, sizeof(line), fp)) {
            if (!strstr(line, name)) continue;
            double c = 0.0, m = 0.0;
            if (sscanf(line, "%*d %lf %lf", &c, &m) == 2) {
                *cpu_percent = c;
                *mem_percent = m;
                found = TRUE;
                break;
            }
        }
    }
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

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    load_sandboxes();

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Sandbox Manager");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    log_buffer = g_queue_new();

    GtkWidget *notebook = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(window), notebook);

    GtkWidget *manager_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), manager_box, gtk_label_new("Sandboxes"));

    // Input fields
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(manager_box), hbox, FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new("Name:");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    entry_name = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox), entry_name, TRUE, TRUE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(manager_box), hbox, FALSE, FALSE, 0);

    label = gtk_label_new("Memory (MB):");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    scale_memory = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1024, 1);
    gtk_range_set_value(GTK_RANGE(scale_memory), 100);
    gtk_scale_set_draw_value(GTK_SCALE(scale_memory), TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), scale_memory, TRUE, TRUE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(manager_box), hbox, FALSE, FALSE, 0);

    label = gtk_label_new("CPU (sec):");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    scale_cpu = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
    gtk_range_set_value(GTK_RANGE(scale_cpu), 10);
    gtk_scale_set_draw_value(GTK_SCALE(scale_cpu), TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), scale_cpu, TRUE, TRUE, 0);

    check_network = gtk_check_button_new_with_label("Enable Network");
    gtk_box_pack_start(GTK_BOX(manager_box), check_network, FALSE, FALSE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(manager_box), hbox, FALSE, FALSE, 0);

    GtkWidget *btn_create = gtk_button_new_with_label("Create Sandbox");
    g_signal_connect(btn_create, "clicked", G_CALLBACK(on_create_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), btn_create, TRUE, TRUE, 0);

    GtkWidget *btn_clear = gtk_button_new_with_label("Clear");
    g_signal_connect(btn_clear, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), btn_clear, TRUE, TRUE, 0);

    GtkWidget *btn_enter = gtk_button_new_with_label("Enter Sandbox");
    g_signal_connect(btn_enter, "clicked", G_CALLBACK(on_enter_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), btn_enter, TRUE, TRUE, 0);

    GtkWidget *btn_delete = gtk_button_new_with_label("Delete Sandbox");
    g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_delete_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), btn_delete, TRUE, TRUE, 0);

    // List
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(manager_box), scrolled, TRUE, TRUE, 0);

    listbox = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);
    update_list();
    g_timeout_add_seconds(2, refresh_usage_cb, NULL);

    // Logs tab
    GtkWidget *logs_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *log_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_container_add(GTK_CONTAINER(log_scrolled), log_view);
    gtk_box_pack_start(GTK_BOX(logs_box), log_scrolled, TRUE, TRUE, 0);

    GtkWidget *log_hint = gtk_label_new("Logs are also written to " LOG_FILE);
    gtk_box_pack_start(GTK_BOX(logs_box), log_hint, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), logs_box, gtk_label_new("Logs"));

    update_log_view();

    gtk_widget_show_all(window);
    gtk_main();

    // Free
    for (GList *l = sandboxes; l; l = l->next) {
        free(l->data);
    }
    g_list_free(sandboxes);

    return 0;
}