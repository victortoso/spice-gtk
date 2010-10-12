#include "spice-widget.h"
#include "spice-audio.h"
#include "spice-common.h"
#include "spice-cmdline.h"

/* config */
static gboolean fullscreen = false;

enum {
    STATE_SCROLL_LOCK,
    STATE_CAPS_LOCK,
    STATE_NUM_LOCK,
    STATE_MAX,
};

typedef struct spice_window spice_window;
typedef struct spice_connection spice_connection;

struct spice_window {
    spice_connection *conn;
    int              id;
    GtkWidget        *toplevel, *spice;
    GtkWidget        *menubar, *toolbar;
    GtkWidget        *hbox, *status, *st[STATE_MAX];
    GtkActionGroup   *ag;
    GtkAccelGroup    *accel;
    GtkUIManager     *ui;
    bool             fullscreen;
    bool             mouse_grabbed;
};

struct spice_connection {
    SpiceSession     *session;
    spice_window     *wins[4];
    GObject          *audio;
    char             *mouse_state;
    char             *agent_state;
    int              channels;
};

static GMainLoop     *mainloop;
static int           connections;

static spice_connection *connection_new(void);
static void connection_destroy(spice_connection *conn);

/* ------------------------------------------------------------------ */

static int ask_user(GtkWidget *parent, char *title, char *message,
                    char *dest, int dlen, int hide)
{
    GtkWidget *dialog, *area, *label, *entry;
    const char *txt;
    int retval;

    /* Create the widgets */
    dialog = gtk_dialog_new_with_buttons(title,
					 parent ? GTK_WINDOW(parent) : NULL,
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_STOCK_OK,
					 GTK_RESPONSE_ACCEPT,
					 GTK_STOCK_CANCEL,
					 GTK_RESPONSE_REJECT,
                                         NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    label = gtk_label_new(message);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(area), label, FALSE, FALSE, 5);

    entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), dest);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    if (hide)
	gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 5);

    /* show and wait for response */
    gtk_widget_show_all(dialog);
    switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
    case GTK_RESPONSE_ACCEPT:
	txt = gtk_entry_get_text(GTK_ENTRY(entry));
	snprintf(dest, dlen, "%s", txt);
	retval = 0;
	break;
    default:
	retval = -1;
	break;
    }
    gtk_widget_destroy(dialog);
    return retval;
}

static int connect_dialog(GtkWidget *parent, SpiceSession *session)
{
    static const struct {
        const char *text;
        const char *prop;
    } entries[] = {
        { .text = "Hostname",   .prop = "host"      },
        { .text = "Port",       .prop = "port"      },
        { .text = "TLS Port",   .prop = "tls-port"  },
    };
    GtkWidget *we[SPICE_N_ELEMENTS(entries)];
    GtkWidget *dialog, *area, *label;
    GtkTable *table;
    const gchar *txt;
    int i, retval;

    /* Create the widgets */
    dialog = gtk_dialog_new_with_buttons("Connect",
					 parent ? GTK_WINDOW(parent) : NULL,
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_STOCK_OK,
					 GTK_RESPONSE_ACCEPT,
					 GTK_STOCK_CANCEL,
					 GTK_RESPONSE_REJECT,
                                         NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    table = GTK_TABLE(gtk_table_new(3, 2, 0));
    gtk_box_pack_start(GTK_BOX(area), GTK_WIDGET(table), TRUE, TRUE, 0);
    gtk_table_set_row_spacings(table, 5);
    gtk_table_set_col_spacings(table, 5);

    for (i = 0; i < SPICE_N_ELEMENTS(entries); i++) {
        label = gtk_label_new(entries[i].text);
        gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
        gtk_table_attach_defaults(table, label, 0, 1, i, i+1);
        we[i] = gtk_entry_new();
        gtk_table_attach_defaults(table, we[i], 1, 2, i, i+1);
        g_object_get(session, entries[i].prop, &txt, NULL);
        if (txt) {
            gtk_entry_set_text(GTK_ENTRY(we[i]), txt);
        }
    }

    /* show and wait for response */
    gtk_widget_show_all(dialog);
    switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
    case GTK_RESPONSE_ACCEPT:
        for (i = 0; i < SPICE_N_ELEMENTS(entries); i++) {
            txt = gtk_entry_get_text(GTK_ENTRY(we[i]));
            g_object_set(session, entries[i].prop, txt, NULL);
        }
	retval = 0;
	break;
    default:
	retval = -1;
	break;
    }
    gtk_widget_destroy(dialog);
    return retval;
}

/* ------------------------------------------------------------------ */

static void update_status(struct spice_window *win)
{
    char status[256];

    if (win == NULL)
        return;
    if (win->mouse_grabbed) {
        snprintf(status, sizeof(status), "Use Shift+F12 to ungrab mouse.");
    } else {
        snprintf(status, sizeof(status), "mouse: %s, agent: %s",
                 win->conn->mouse_state, win->conn->agent_state);
    }
    gtk_label_set_text(GTK_LABEL(win->status), status);
}

static void menu_cb_connect(GtkAction *action, void *data)
{
    struct spice_connection *conn;

    conn = connection_new();
    spice_session_connect(conn->session);
}

static void menu_cb_close(GtkAction *action, void *data)
{
    struct spice_window *win = data;

    spice_session_disconnect(win->conn->session);
}

static void menu_cb_copy(GtkAction *action, void *data)
{
    struct spice_window *win = data;

    spice_display_copy_to_guest(win->spice);
}

static void menu_cb_paste(GtkAction *action, void *data)
{
    struct spice_window *win = data;

    spice_display_paste_from_guest(win->spice);
}

static void menu_cb_fullscreen(GtkAction *action, void *data)
{
    struct spice_window *win = data;

    if (win->fullscreen) {
        gtk_window_unfullscreen(GTK_WINDOW(win->toplevel));
    } else {
        gtk_window_fullscreen(GTK_WINDOW(win->toplevel));
    }
}

static void menu_cb_ungrab(GtkAction *action, void *data)
{
    struct spice_window *win = data;

    spice_display_mouse_ungrab(win->spice);
}

static void menu_cb_bool_prop(GtkToggleAction *action, gpointer data)
{
    struct spice_window *win = data;
    gboolean state = gtk_toggle_action_get_active(action);

    fprintf(stderr, "%s: %s = %s\n", __FUNCTION__,
            gtk_action_get_name(GTK_ACTION(action)), state ? "yes" : "no");
    g_object_set(G_OBJECT(win->spice),
                 gtk_action_get_name(GTK_ACTION(action)), state,
                 NULL);
}

static void menu_cb_about(GtkAction *action, void *data)
{
    static char *comments = "gtk client app for the\n"
        "spice remote desktop protocol";
    static char *copyright = "(c) 2010 Red Hat";
    static char *website = "http://www.spice-space.org";
    static char *authors[] = { "Gerd Hoffmann <kraxel@redhat.com>", NULL };
    struct spice_window *win = data;

    gtk_show_about_dialog(GTK_WINDOW(win->toplevel),
                          "authors",         authors,
                          "comments",        comments,
                          "copyright",       copyright,
                          "logo-icon-name",  GTK_STOCK_ABOUT,
			  "website",         website,
//                        "version",         VERSION,
//			  "license",         "GPLv2+",
                          NULL);
}

static gboolean delete_cb(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    struct spice_window *win = data;

    spice_session_disconnect(win->conn->session);
    return true;
}

static void destroy_cb(GtkWidget *widget, gpointer data)
{
#if 0
    struct spice_window *win = data;

    if (win->id == 0) {
        g_main_loop_quit(mainloop);
    }
#endif
}

static gboolean window_state_cb(GtkWidget *widget, GdkEventWindowState *event,
				gpointer data)
{
    struct spice_window *win = data;

    if (event->changed_mask & GDK_WINDOW_STATE_FULLSCREEN) {
        win->fullscreen = event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN;
        if (win->fullscreen) {
            gtk_widget_hide(win->menubar);
            gtk_widget_hide(win->toolbar);
            gtk_widget_hide(win->hbox);
            gtk_widget_grab_focus(win->spice);
        } else {
            gtk_widget_show(win->menubar);
            gtk_widget_show(win->toolbar);
            gtk_widget_show(win->hbox);
        }
    }
    return TRUE;
}

static void mouse_grab_cb(GtkWidget *widget, gint grabbed, gpointer data)
{
    struct spice_window *win = data;

    win->mouse_grabbed = grabbed;
    update_status(win);
}

/* ------------------------------------------------------------------ */

static const GtkActionEntry entries[] = {
    {
	.name        = "FileMenu",
	.label       = "_File",
    },{
	.name        = "EditMenu",
	.label       = "_Edit",
    },{
	.name        = "ViewMenu",
	.label       = "_View",
    },{
	.name        = "InputMenu",
	.label       = "_Input",
    },{
	.name        = "OptionMenu",
	.label       = "_Options",
    },{
	.name        = "HelpMenu",
	.label       = "_Help",
    },{

	/* File menu */
	.name        = "Connect",
	.stock_id    = GTK_STOCK_CONNECT,
	.label       = "_Connect ...",
	.callback    = G_CALLBACK(menu_cb_connect),
        .accelerator = "", /* none (disable default "<control>Q") */
    },{
	.name        = "Close",
	.stock_id    = GTK_STOCK_CLOSE,
	.label       = "_Close",
	.callback    = G_CALLBACK(menu_cb_close),
//        .accelerator = "", /* none (disable default "<control>Q") */
    },{

	/* Edit menu */
	.name        = "CopyToGuest",
	.stock_id    = GTK_STOCK_COPY,
	.label       = "_Copy to guest",
	.callback    = G_CALLBACK(menu_cb_copy),
        .accelerator = "", /* none (disable default "<control>Q") */
    },{
	.name        = "PasteFromGuest",
	.stock_id    = GTK_STOCK_PASTE,
	.label       = "_Paste from guest",
	.callback    = G_CALLBACK(menu_cb_paste),
        .accelerator = "", /* none (disable default "<control>Q") */
    },{

	/* View menu */
	.name        = "Fullscreen",
	.stock_id    = GTK_STOCK_FULLSCREEN,
	.label       = "_Fullscreen",
	.callback    = G_CALLBACK(menu_cb_fullscreen),
        .accelerator = "<shift>F11",
    },{

	/* Input menu */
	.name        = "UngrabMouse",
	.label       = "_Ungrab mouse",
	.callback    = G_CALLBACK(menu_cb_ungrab),
        .accelerator = "<shift>F12",
    },{

	/* Help menu */
	.name        = "About",
	.stock_id    = GTK_STOCK_ABOUT,
	.label       = "_About ...",
	.callback    = G_CALLBACK(menu_cb_about),
    }
};

static const GtkToggleActionEntry tentries[] = {
    {
	.name        = "grab-keyboard",
	.label       = "Grab keyboard when active and focused",
	.callback    = G_CALLBACK(menu_cb_bool_prop),
    },{
	.name        = "grab-mouse",
	.label       = "Grab mouse in server mode (no tabled/vdagent)",
	.callback    = G_CALLBACK(menu_cb_bool_prop),
    },{
	.name        = "resize-guest",
	.label       = "Resize guest to match window size",
	.callback    = G_CALLBACK(menu_cb_bool_prop),
    },{
	.name        = "auto-clipboard",
	.label       = "Automagic clipboard sharing between host and guest",
	.callback    = G_CALLBACK(menu_cb_bool_prop),
    }
};

static char ui_xml[] =
"<ui>\n"
"  <menubar action='MainMenu'>\n"
"    <menu action='FileMenu'>\n"
"      <menuitem action='Connect'/>\n"
"      <menuitem action='Close'/>\n"
"    </menu>\n"
"    <menu action='EditMenu'>\n"
"      <menuitem action='CopyToGuest'/>\n"
"      <menuitem action='PasteFromGuest'/>\n"
"    </menu>\n"
"    <menu action='ViewMenu'>\n"
"      <menuitem action='Fullscreen'/>\n"
"    </menu>\n"
"    <menu action='InputMenu'>\n"
"      <menuitem action='UngrabMouse'/>\n"
"    </menu>\n"
"    <menu action='OptionMenu'>\n"
"      <menuitem action='grab-keyboard'/>\n"
"      <menuitem action='grab-mouse'/>\n"
"      <menuitem action='resize-guest'/>\n"
"      <menuitem action='auto-clipboard'/>\n"
"    </menu>\n"
"    <menu action='HelpMenu'>\n"
"      <menuitem action='About'/>\n"
"    </menu>\n"
"  </menubar>\n"
"  <toolbar action='ToolBar'>\n"
"    <toolitem action='Close'/>\n"
"    <separator/>\n"
"    <toolitem action='Fullscreen'/>\n"
"  </toolbar>\n"
"</ui>\n";

static spice_window *create_spice_window(spice_connection *conn, int id)
{
    char title[32];
    struct spice_window *win;
    GtkWidget *vbox, *frame;
    GError *err = NULL;
    int i;

    win = malloc(sizeof(*win));
    if (NULL == win)
        return NULL;
    memset(win,0,sizeof(*win));
    win->id = id;
    win->conn = conn;
    fprintf(stderr, "create window (#%d)\n", win->id);

    /* toplevel */
    win->toplevel = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    snprintf(title, sizeof(title), "spice display %d", id);
    gtk_window_set_title(GTK_WINDOW(win->toplevel), title);
    g_signal_connect(G_OBJECT(win->toplevel), "window-state-event",
		     G_CALLBACK(window_state_cb), win);
    g_signal_connect(G_OBJECT(win->toplevel), "delete-event",
		     G_CALLBACK(delete_cb), win);
    g_signal_connect(G_OBJECT(win->toplevel), "destroy",
                     G_CALLBACK(destroy_cb), win);

    /* menu + toolbar */
    win->ui = gtk_ui_manager_new();
    win->ag = gtk_action_group_new("MenuActions");
    gtk_action_group_add_actions(win->ag, entries, G_N_ELEMENTS(entries), win);
    gtk_action_group_add_toggle_actions(win->ag, tentries,
					G_N_ELEMENTS(tentries), win);
    gtk_ui_manager_insert_action_group(win->ui, win->ag, 0);
    win->accel = gtk_ui_manager_get_accel_group(win->ui);
    gtk_window_add_accel_group(GTK_WINDOW(win->toplevel), win->accel);

    err = NULL;
    if (!gtk_ui_manager_add_ui_from_string(win->ui, ui_xml, -1, &err)) {
	g_message("building menus failed: %s", err->message);
	g_error_free(err);
	exit(1);
    }
    win->menubar = gtk_ui_manager_get_widget(win->ui, "/MainMenu");
    win->toolbar = gtk_ui_manager_get_widget(win->ui, "/ToolBar");

    /* spice display */
    win->spice = spice_display_new(conn->session, id);
    g_signal_connect(G_OBJECT(win->spice), "spice-display-mouse-grab",
		     G_CALLBACK(mouse_grab_cb), win);

    /* status line */
    win->hbox = gtk_hbox_new(FALSE, 1);

    win->status = gtk_label_new("status line");
    gtk_misc_set_alignment(GTK_MISC(win->status), 0, 0.5);
    gtk_misc_set_padding(GTK_MISC(win->status), 3, 1);
    update_status(win);

    frame = gtk_frame_new(NULL);
    gtk_box_pack_start(GTK_BOX(win->hbox), frame, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(frame), win->status);

    for (i = 0; i < STATE_MAX; i++) {
        win->st[i] = gtk_label_new("?");
        gtk_label_set_width_chars(GTK_LABEL(win->st[i]), 5);
        frame = gtk_frame_new(NULL);
        gtk_box_pack_end(GTK_BOX(win->hbox), frame, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(frame), win->st[i]);
    }

    /* Make a vbox and put stuff in */
    vbox = gtk_vbox_new(FALSE, 1);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 0);
    gtk_container_add(GTK_CONTAINER(win->toplevel), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), win->menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), win->toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), win->spice, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(vbox), win->hbox, FALSE, TRUE, 0);


    /* init toggle actions */
    for (i = 0; i < G_N_ELEMENTS(tentries); i++) {
        GtkAction *toggle;
        gboolean state;
        toggle = gtk_action_group_get_action(win->ag, tentries[i].name);
        g_object_get(win->spice, tentries[i].name, &state, NULL);
        gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(toggle), state);
    }

    /* show window */
    if (fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(win->toplevel));
    gtk_widget_show_all(win->toplevel);
    gtk_widget_grab_focus(win->spice);
    return win;
}

static void destroy_spice_window(spice_window *win)
{
    fprintf(stderr, "destroy window (#%d)\n", win->id);
    gtk_widget_destroy(win->toplevel);
    free(win);
}

/* ------------------------------------------------------------------ */

static void main_channel_event(SpiceChannel *channel, enum SpiceChannelEvent event,
                               gpointer data)
{
    spice_connection *conn = data;
    char password[64];
    int rc;

    switch (event) {
    case SPICE_CHANNEL_OPENED:
        fprintf(stderr, "main channel: opened\n");
        /* nothing */
        break;
    case SPICE_CHANNEL_CLOSED:
        fprintf(stderr, "main channel: closed\n");
        /* nothing */
        break;
    case SPICE_CHANNEL_ERROR_CONNECT:
        fprintf(stderr, "main channel: failed to connect\n");
        rc = connect_dialog(NULL, conn->session);
        if (rc == 0) {
            spice_session_connect(conn->session);
        } else {
            spice_session_disconnect(conn->session);
        }
        break;
    case SPICE_CHANNEL_ERROR_AUTH:
        fprintf(stderr, "main channel: auth failure (wrong password?)\n");
        strcpy(password, "");
        rc = ask_user(NULL, "Authentication",
                      "Please enter the spice server password",
                      password, sizeof(password), true);
        if (rc == 0) {
            g_object_set(conn->session, "password", password, NULL);
            spice_session_connect(conn->session);
        } else {
            spice_session_disconnect(conn->session);
        }
        break;
    default:
        /* TODO: more sophisticated error handling */
        fprintf(stderr, "unknown main channel event: %d\n", event);
        spice_session_disconnect(conn->session);
        break;
    }
}

static void main_mouse_update(SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    gint mode;

    g_object_get(channel, "mouse-mode", &mode, NULL);
    switch (mode) {
    case SPICE_MOUSE_MODE_SERVER:
        conn->mouse_state = "server";
        break;
    case SPICE_MOUSE_MODE_CLIENT:
        conn->mouse_state = "client";
        break;
    default:
        conn->mouse_state = "?";
        break;
    }
    update_status(conn->wins[0]);
}

static void main_agent_update(SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    gboolean agent_connected;

    g_object_get(channel, "agent-connected", &agent_connected, NULL);
    conn->agent_state = agent_connected ? "yes" : "no";
    update_status(conn->wins[0]);
}

static void inputs_modifiers(SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    int m;

    g_object_get(channel, "key-modifiers", &m, NULL);
    gtk_label_set_text(GTK_LABEL(conn->wins[0]->st[STATE_SCROLL_LOCK]),
                       m & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK ? "SCROLL" : "");
    gtk_label_set_text(GTK_LABEL(conn->wins[0]->st[STATE_CAPS_LOCK]),
                       m & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK ? "CAPS" : "");
    gtk_label_set_text(GTK_LABEL(conn->wins[0]->st[STATE_NUM_LOCK]),
                       m & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK ? "NUM" : "");
}

static void channel_new(SpiceSession *s, SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    int id = spice_channel_id(channel);

    conn->channels++;

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        fprintf(stderr, "new main channel\n");
        g_signal_connect(channel, "spice-channel-event",
                         G_CALLBACK(main_channel_event), conn);
        g_signal_connect(channel, "spice-main-mouse-update",
                         G_CALLBACK(main_mouse_update), conn);
        g_signal_connect(channel, "spice-main-agent-update",
                         G_CALLBACK(main_agent_update), conn);
        main_mouse_update(channel, conn);
        main_agent_update(channel, conn);
    }

    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        if (id >= SPICE_N_ELEMENTS(conn->wins))
            return;
        if (conn->wins[id] != NULL)
            return;
        fprintf(stderr, "new display channel (#%d)\n", id);
        conn->wins[id] = create_spice_window(conn, id);
    }

    if (SPICE_IS_INPUTS_CHANNEL(channel)) {
        fprintf(stderr, "new inputs channel\n");
        g_signal_connect(channel, "spice-inputs-modifiers",
                         G_CALLBACK(inputs_modifiers), conn);
    }

    if (SPICE_IS_PLAYBACK_CHANNEL(channel)) {
        if (conn->audio != NULL)
            return;
        fprintf(stderr, "new audio channel\n");
        conn->audio = spice_audio_new(s, mainloop, "spice");
    }
}

static void channel_destroy(SpiceSession *s, SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    int id = spice_channel_id(channel);

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        fprintf(stderr, "zap main channel\n");
    }

    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        if (id >= SPICE_N_ELEMENTS(conn->wins))
            return;
        if (conn->wins[id] == NULL)
            return;
        fprintf(stderr, "zap display channel (#%d)\n", id);
        destroy_spice_window(conn->wins[id]);
        conn->wins[id] = NULL;
    }

    if (SPICE_IS_PLAYBACK_CHANNEL(channel)) {
        if (conn->audio == NULL)
            return;
        fprintf(stderr, "zap audio channel\n");
        /* zap audio */
    }

    conn->channels--;
    if (conn->channels > 0) {
        return;
    }

    connection_destroy(conn);
}

static spice_connection *connection_new(void)
{
    spice_connection *conn;

    fprintf(stderr, "%s\n", __FUNCTION__);
    conn = spice_new0(spice_connection, 1);
    conn->session = spice_session_new();
    g_signal_connect(conn->session, "spice-session-channel-new",
                     G_CALLBACK(channel_new), conn);
    g_signal_connect(conn->session, "spice-session-channel-destroy",
                     G_CALLBACK(channel_destroy), conn);
    connections++;
    return conn;
}

static void connection_destroy(spice_connection *conn)
{
    fprintf(stderr, "%s\n", __FUNCTION__);
    free(conn);
    connections--;
    g_main_loop_quit(mainloop);
}

/* ------------------------------------------------------------------ */

static GOptionEntry cmd_entries[] = {
    {
        .long_name        = "full-screen",
        .short_name       = 'f',
        .arg              = G_OPTION_ARG_NONE,
        .arg_data         = &fullscreen,
        .description      = "open in full screen mode",
    },{
        /* end of list */
    }
};

int main(int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;
    spice_connection *conn;

    /* parse opts */
    gtk_init(&argc, &argv);
    context = g_option_context_new("- spice client application");
    g_option_context_add_main_entries (context, cmd_entries, NULL);
    g_option_context_add_group(context, gtk_get_option_group(TRUE));
    g_option_context_add_group(context, spice_cmdline_get_option_group());
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_print ("option parsing failed: %s\n", error->message);
        exit (1);
    }

    g_type_init();
    mainloop = g_main_loop_new(NULL, false);

    conn = connection_new();
    spice_cmdline_session_setup(conn->session);
    spice_session_connect(conn->session);

    while (connections) {
        g_main_loop_run(mainloop);
        fprintf(stderr, "%s: %d connections\n", __FUNCTION__, connections);
    }
    return 0;
}
