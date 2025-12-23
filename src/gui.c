#include <gtk/gtk.h>
#include <vte/vte.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_FILE "/home/ubuntu/sandbox/sandboxes.txt"
#define SANDBOX_BIN "/home/ubuntu/sandbox/bin/sandbox"

typedef struct {
    char name[256];
    int memory;
    int cpu;
    int network;
    time_t date;
} Sandbox;

GtkWidget *entry_name;
GtkWidget *entry_memory;
GtkWidget *entry_cpu;
GtkWidget *check_network;
GtkWidget *listbox;
GList *sandboxes = NULL;

typedef struct {
    GtkWindow *window;
    char *sandbox_name;
} TerminalContext;

static void on_terminal_mapped(GtkWidget *terminal, gpointer user_data);
static void on_spawn_ready(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data);
static void show_error_dialog(GtkWindow *parent, const char *msg, GError *err);
static void free_terminal_ctx(gpointer data);

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
        GtkWidget *label = gtk_label_new(NULL);
        char text[1024];
        sprintf(text, "Name: %s, Memory: %d MB, CPU: %d s, Network: %s, Date: %s",
                s->name, s->memory, s->cpu, s->network ? "Yes" : "No", buf);
        gtk_label_set_text(GTK_LABEL(label), text);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), label, -1);
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
        if (ctx && ctx->window) {
            gtk_widget_destroy(GTK_WIDGET(ctx->window));
        }
        return;
    }
    (void)pid; // child-exited signal will close the window
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

void on_create_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry_name));
    const char *mem_str = gtk_entry_get_text(GTK_ENTRY(entry_memory));
    const char *cpu_str = gtk_entry_get_text(GTK_ENTRY(entry_cpu));
    int network = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_network));

    if (!name || !*name || !mem_str || !*mem_str || !cpu_str || !*cpu_str) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Please fill all fields");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    int memory = atoi(mem_str);
    int cpu = atoi(cpu_str);
    if (memory <= 0 || cpu <= 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Invalid numeric values");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (!ensure_root(NULL)) return;

    // Build argv dynamically to avoid empty arguments
    char *argv_cmd[10] = {0};
    int idx = 0;
    argv_cmd[idx++] = SANDBOX_BIN;
    argv_cmd[idx++] = "-c";
    argv_cmd[idx++] = "-m";
    argv_cmd[idx++] = (char *)mem_str;
    argv_cmd[idx++] = "-t";
    argv_cmd[idx++] = (char *)cpu_str;
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

    GtkWidget *label = gtk_bin_get_child(GTK_BIN(row));
    const char *text = gtk_label_get_text(GTK_LABEL(label));
    char name[256];
    sscanf(text, "Name: %[^,]", name);

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
    gtk_entry_set_text(GTK_ENTRY(entry_memory), "100");
    gtk_entry_set_text(GTK_ENTRY(entry_cpu), "10");
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

    GtkWidget *label = gtk_bin_get_child(GTK_BIN(row));
    const char *text = gtk_label_get_text(GTK_LABEL(label));
    char name[256];
    sscanf(text, "Name: %[^,]", name);

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
    }
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    load_sandboxes();

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Sandbox Manager");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Input fields
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new("Name:");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    entry_name = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox), entry_name, TRUE, TRUE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    label = gtk_label_new("Memory (MB):");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    entry_memory = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_memory), "100");
    gtk_box_pack_start(GTK_BOX(hbox), entry_memory, TRUE, TRUE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    label = gtk_label_new("CPU (sec):");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    entry_cpu = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_cpu), "10");
    gtk_box_pack_start(GTK_BOX(hbox), entry_cpu, TRUE, TRUE, 0);

    check_network = gtk_check_button_new_with_label("Enable Network");
    gtk_box_pack_start(GTK_BOX(vbox), check_network, FALSE, FALSE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

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
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    listbox = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);
    update_list();

    gtk_widget_show_all(window);
    gtk_main();

    // Free
    for (GList *l = sandboxes; l; l = l->next) {
        free(l->data);
    }
    g_list_free(sandboxes);

    return 0;
}