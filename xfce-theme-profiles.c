/*
 * xfce-theme-profiles — Perfis de tema para o XFCE
 *
 * Cria perfis que combinam:
 *   - tema GTK            (xfconf: xsettings /Net/ThemeName)
 *   - tema de janelas     (xfconf: xfwm4 /general/theme)
 *   - tema de ícones      (xfconf: xsettings /Net/IconThemeName)
 *   - tema do rofi        (~/.config/rofi/config.rasi, linha @theme)
 *   - LightDM (opcional)  (/etc/lightdm/lightdm-gtk-greeter.conf via pkexec:
 *                          tema próprio do greeter e/ou GTK + ícones do perfil)
 *   - esquema do xfce4-terminal (~/.config/xfce4/terminal/terminalrc)
 *   - esquema do Mousepad (GSettings: org.xfce.mousepad.preferences.view color-scheme)
 * e permite alternar entre eles com um clique (ou pela linha de comando).
 *
 * Aparece no Gerenciador de Configurações do XFCE (categoria Pessoal).
 *
 * Compilar:  make
 * Instalar:  sudo make install
 *
 * CLI:
 *   xfce-theme-profiles --list
 *   xfce-theme-profiles --apply "Nome do perfil"
 *   xfce-theme-profiles --active
 */

#include <gtk/gtk.h>
#include <xfconf/xfconf.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifndef APP_VERSION
#define APP_VERSION   "1.2.0"
#endif
#define LIGHTDM_DIR   "/etc/lightdm"
#define LIGHTDM_CONF  "/etc/lightdm/lightdm-gtk-greeter.conf"
#define APP_NAME      "xfce-theme-profiles"
#define PROFILE_PFX   "Profile:"
#define NONE_LABEL    "(não alterar)"
#define MP_SCHEMA     "org.xfce.mousepad.preferences.view"
#define MP_KEY        "color-scheme"

static GKeyFile *kf = NULL;          /* perfis */
static char     *cfg_path = NULL;
static gboolean  xfconf_ok = FALSE;

/* ------------------------------------------------------------------ */
/* Itens detectados (temas instalados)                                 */
/* ------------------------------------------------------------------ */

typedef struct { char *id, *label, *path; } Item;

static void item_free(gpointer p)
{
    Item *i = p;
    g_free(i->id); g_free(i->label); g_free(i->path); g_free(i);
}

static gint item_cmp(gconstpointer pa, gconstpointer pb)
{
    const Item *a = *(Item * const *)pa, *b = *(Item * const *)pb;
    return g_utf8_collate(a->label, b->label);
}

static GPtrArray *items_new(void) { return g_ptr_array_new_with_free_func(item_free); }

static void items_add(GPtrArray *a, const char *id, const char *label, const char *path)
{
    for (guint i = 0; i < a->len; i++)
        if (g_strcmp0(((Item *)a->pdata[i])->id, id) == 0) return;
    Item *it = g_new0(Item, 1);
    it->id = g_strdup(id);
    it->label = g_strdup(label ? label : id);
    it->path = g_strdup(path);
    g_ptr_array_add(a, it);
}

/* diretórios de busca: legado (~/.themes), dados do usuário, dados do sistema */
static GPtrArray *search_dirs(const char *sub, const char *legacy)
{
    GPtrArray *d = g_ptr_array_new_with_free_func(g_free);
    if (legacy) g_ptr_array_add(d, g_build_filename(g_get_home_dir(), legacy, NULL));
    g_ptr_array_add(d, g_build_filename(g_get_user_data_dir(), sub, NULL));
    for (const char * const *s = g_get_system_data_dirs(); *s; s++)
        g_ptr_array_add(d, g_build_filename(*s, sub, NULL));
    return d;
}

static GPtrArray *scan_gtk(void)
{
    GPtrArray *a = items_new(), *dirs = search_dirs("themes", ".themes");
    for (guint i = 0; i < dirs->len; i++) {
        GDir *g = g_dir_open(dirs->pdata[i], 0, NULL);
        if (!g) continue;
        const char *n;
        while ((n = g_dir_read_name(g))) {
            char *p = g_build_filename(dirs->pdata[i], n, "gtk-3.0", NULL);
            if (g_file_test(p, G_FILE_TEST_IS_DIR)) items_add(a, n, n, NULL);
            g_free(p);
        }
        g_dir_close(g);
    }
    g_ptr_array_unref(dirs);
    g_ptr_array_sort(a, item_cmp);
    return a;
}

static GPtrArray *scan_icons(void)
{
    GPtrArray *a = items_new(), *dirs = search_dirs("icons", ".icons");
    for (guint i = 0; i < dirs->len; i++) {
        GDir *g = g_dir_open(dirs->pdata[i], 0, NULL);
        if (!g) continue;
        const char *n;
        while ((n = g_dir_read_name(g))) {
            if (!strcmp(n, "hicolor") || !strcmp(n, "locolor")) continue;
            char *p = g_build_filename(dirs->pdata[i], n, "index.theme", NULL);
            GKeyFile *k = g_key_file_new();
            if (g_key_file_load_from_file(k, p, 0, NULL) &&
                (g_key_file_has_key(k, "Icon Theme", "Directories", NULL) ||
                 g_key_file_has_key(k, "Icon Theme", "ScaledDirectories", NULL)) &&
                !g_key_file_get_boolean(k, "Icon Theme", "Hidden", NULL))
                items_add(a, n, n, NULL);
            g_key_file_free(k);
            g_free(p);
        }
        g_dir_close(g);
    }
    g_ptr_array_unref(dirs);
    g_ptr_array_sort(a, item_cmp);
    return a;
}

static GPtrArray *scan_terminal(void)
{
    GPtrArray *a = items_new(), *dirs = search_dirs("xfce4/terminal/colorschemes", NULL);
    for (guint i = 0; i < dirs->len; i++) {
        GDir *g = g_dir_open(dirs->pdata[i], 0, NULL);
        if (!g) continue;
        const char *n;
        while ((n = g_dir_read_name(g))) {
            if (!g_str_has_suffix(n, ".theme")) continue;
            char *p = g_build_filename(dirs->pdata[i], n, NULL);
            GKeyFile *k = g_key_file_new();
            if (g_key_file_load_from_file(k, p, 0, NULL)) {
                char *nm = g_key_file_get_string(k, "Scheme", "Name", NULL);
                if (nm) items_add(a, nm, nm, p);
                g_free(nm);
            }
            g_key_file_free(k);
            g_free(p);
        }
        g_dir_close(g);
    }
    g_ptr_array_unref(dirs);
    g_ptr_array_sort(a, item_cmp);
    return a;
}

/* extrai attr="valor" de uma tag XML simples [tag, end) */
static char *xml_attr(const char *tag, const char *end, const char *attr)
{
    size_t al = strlen(attr);
    const char *p = tag;
    while (p < end && (p = g_strstr_len(p, end - p, attr)) != NULL) {
        if (p > tag && g_ascii_isspace(p[-1]) && p + al + 1 < end && p[al] == '=') {
            char q = p[al + 1];
            if (q == '"' || q == '\'') {
                const char *s = p + al + 2;
                const char *e = memchr(s, q, end - s);
                if (e) return g_strndup(s, e - s);
            }
        }
        p += al;
    }
    return NULL;
}

static GPtrArray *scan_mousepad(void)
{
    static const char *subs[] = { "gtksourceview-3.0/styles", "gtksourceview-4/styles",
                                  "gtksourceview-5/styles", NULL };
    GPtrArray *a = items_new();
    for (int s = 0; subs[s]; s++) {
        GPtrArray *dirs = search_dirs(subs[s], NULL);
        for (guint i = 0; i < dirs->len; i++) {
            GDir *g = g_dir_open(dirs->pdata[i], 0, NULL);
            if (!g) continue;
            const char *n;
            while ((n = g_dir_read_name(g))) {
                if (!g_str_has_suffix(n, ".xml")) continue;
                char *p = g_build_filename(dirs->pdata[i], n, NULL), *txt = NULL;
                gsize len = 0;
                if (g_file_get_contents(p, &txt, &len, NULL)) {
                    const char *t = g_strstr_len(txt, len, "<style-scheme");
                    const char *e = t ? memchr(t, '>', len - (t - txt)) : NULL;
                    if (t && e) {
                        char *id = xml_attr(t, e, "id");
                        char *nm = xml_attr(t, e, "name");
                        if (!nm) nm = xml_attr(t, e, "_name");
                        if (id) items_add(a, id, nm ? nm : id, p);
                        g_free(id); g_free(nm);
                    }
                    g_free(txt);
                }
                g_free(p);
            }
            g_dir_close(g);
        }
        g_ptr_array_unref(dirs);
    }
    g_ptr_array_sort(a, item_cmp);
    return a;
}

static GPtrArray *scan_xfwm4(void)
{
    GPtrArray *a = items_new(), *dirs = search_dirs("themes", ".themes");
    for (guint i = 0; i < dirs->len; i++) {
        GDir *g = g_dir_open(dirs->pdata[i], 0, NULL);
        if (!g) continue;
        const char *n;
        while ((n = g_dir_read_name(g))) {
            char *p = g_build_filename(dirs->pdata[i], n, "xfwm4", "themerc", NULL);
            if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) items_add(a, n, n, NULL);
            g_free(p);
        }
        g_dir_close(g);
    }
    g_ptr_array_unref(dirs);
    g_ptr_array_sort(a, item_cmp);
    return a;
}

/* temas do rofi: por nome (pastas de temas do rofi) ou por caminho (arquivos .rasi soltos em ~/.config/rofi) */
static GPtrArray *scan_rofi(void)
{
    GPtrArray *a = items_new();
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(dirs, g_build_filename(g_get_user_config_dir(), "rofi", "themes", NULL));
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "rofi", "themes", NULL));
    for (const char * const *s = g_get_system_data_dirs(); *s; s++)
        g_ptr_array_add(dirs, g_build_filename(*s, "rofi", "themes", NULL));

    for (guint i = 0; i < dirs->len; i++) {
        GDir *g = g_dir_open(dirs->pdata[i], 0, NULL);
        if (!g) continue;
        const char *n;
        while ((n = g_dir_read_name(g))) {
            if (!g_str_has_suffix(n, ".rasi")) continue;
            char *id = g_strndup(n, strlen(n) - 5);
            items_add(a, id, id, NULL);
            g_free(id);
        }
        g_dir_close(g);
    }
    g_ptr_array_unref(dirs);

    /* arquivos soltos em ~/.config/rofi (ex.: arc-dark.rasi) — usados por caminho */
    char *cd = g_build_filename(g_get_user_config_dir(), "rofi", NULL);
    GDir *g = g_dir_open(cd, 0, NULL);
    if (g) {
        const char *n;
        while ((n = g_dir_read_name(g))) {
            if (!g_str_has_suffix(n, ".rasi") || !strcmp(n, "config.rasi")) continue;
            char *path = g_build_filename(cd, n, NULL);
            char *lbl = g_strdup_printf("%.*s (~/.config/rofi)", (int)(strlen(n) - 5), n);
            items_add(a, path, lbl, path);
            g_free(path); g_free(lbl);
        }
        g_dir_close(g);
    }
    g_free(cd);
    g_ptr_array_sort(a, item_cmp);
    return a;
}

/* ------------------------------------------------------------------ */
/* Aplicação de cada componente                                        */
/* ------------------------------------------------------------------ */

static gboolean mousepad_available(void)
{
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (!src) return FALSE;
    GSettingsSchema *sc = g_settings_schema_source_lookup(src, MP_SCHEMA, TRUE);
    if (!sc) return FALSE;
    gboolean ok = g_settings_schema_has_key(sc, MP_KEY);
    g_settings_schema_unref(sc);
    return ok;
}

static gboolean xf_set(const char *chan, const char *prop, const char *val, GError **err)
{
    if (!xfconf_ok) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "xfconf indisponível");
        return FALSE;
    }
    XfconfChannel *c = xfconf_channel_get(chan);
    if (!xfconf_channel_set_string(c, prop, val)) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "não foi possível gravar %s/%s", chan, prop);
        return FALSE;
    }
    return TRUE;
}

static char *xf_get(const char *chan, const char *prop)
{
    if (!xfconf_ok) return g_strdup("");
    return xfconf_channel_get_string(xfconf_channel_get(chan), prop, "");
}

/* ---- rofi: ~/.config/rofi/config.rasi (linha @theme "...") ---- */

static char *rofi_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "rofi", "config.rasi", NULL);
}

static char *capture_rofi(void)
{
    char *p = rofi_config_path(), *txt = NULL, *res = g_strdup("");
    if (g_file_get_contents(p, &txt, NULL, NULL)) {
        char **lines = g_strsplit(txt, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            const char *s = lines[i];
            while (g_ascii_isspace(*s)) s++;
            if (g_str_has_prefix(s, "@theme") && (s[6] == ' ' || s[6] == '\t' || s[6] == '"')) {
                const char *q1 = strchr(s, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                if (q1 && q2) { g_free(res); res = g_strndup(q1 + 1, q2 - q1 - 1); }
                break;
            }
        }
        g_strfreev(lines);
        g_free(txt);
    }
    g_free(p);
    return res;
}

static gboolean apply_rofi(const char *id, GError **err)
{
    char *p = rofi_config_path(), *dir = g_path_get_dirname(p), *txt = NULL;
    g_mkdir_with_parents(dir, 0755);
    g_file_get_contents(p, &txt, NULL, NULL);   /* pode não existir */

    char *theme_line = g_strdup_printf("@theme \"%s\"", id);
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    gboolean done = FALSE;
    if (txt) {
        char **lines = g_strsplit(txt, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            const char *s = lines[i];
            while (g_ascii_isspace(*s)) s++;
            if (g_str_has_prefix(s, "@theme") && (s[6] == ' ' || s[6] == '\t' || s[6] == '"')) {
                if (!done) { g_ptr_array_add(out, g_strdup(theme_line)); done = TRUE; }
                continue;                       /* descarta @theme antigos/duplicados */
            }
            g_ptr_array_add(out, g_strdup(lines[i]));
        }
        g_strfreev(lines);
    }
    if (!done) g_ptr_array_insert(out, 0, g_strdup(theme_line));
    g_ptr_array_add(out, NULL);

    char *data = g_strjoinv("\n", (char **)out->pdata);
    if (!g_str_has_suffix(data, "\n")) {
        char *tmp = g_strconcat(data, "\n", NULL);
        g_free(data); data = tmp;
    }
    gboolean ok = g_file_set_contents(p, data, -1, err);

    g_free(data); g_ptr_array_unref(out); g_free(theme_line);
    g_free(txt); g_free(dir); g_free(p);
    return ok;
}

static char *terminalrc_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "xfce4", "terminal", "terminalrc", NULL);
}

static char *find_item_path(GPtrArray *items, const char *id)
{
    for (guint i = 0; i < items->len; i++) {
        Item *it = items->pdata[i];
        if (!strcmp(it->id, id)) return g_strdup(it->path);
    }
    return NULL;
}

static gboolean apply_terminal(const char *name, GError **err)
{
    static const char *drop[] = {
        "ColorForeground", "ColorBackground", "ColorCursor", "ColorCursorForeground",
        "ColorCursorUseDefault", "ColorSelection", "ColorSelectionBackground",
        "ColorSelectionUseDefault", "ColorBold", "ColorBoldUseDefault", "ColorPalette",
        "ColorPaletteName", NULL };
    GPtrArray *items = scan_terminal();
    char *path = find_item_path(items, name);
    g_ptr_array_unref(items);
    if (!path) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "esquema do terminal '%s' não encontrado", name);
        return FALSE;
    }

    GKeyFile *scheme = g_key_file_new();
    if (!g_key_file_load_from_file(scheme, path, 0, err)) {
        g_key_file_free(scheme); g_free(path); return FALSE;
    }

    char *rcp = terminalrc_path(), *dir = g_path_get_dirname(rcp);
    g_mkdir_with_parents(dir, 0700);
    GKeyFile *rc = g_key_file_new();
    g_key_file_load_from_file(rc, rcp, G_KEY_FILE_KEEP_COMMENTS, NULL); /* pode não existir */

    for (int i = 0; drop[i]; i++)
        g_key_file_remove_key(rc, "Configuration", drop[i], NULL);

    gsize n = 0;
    char **keys = g_key_file_get_keys(scheme, "Scheme", &n, NULL);
    for (gsize i = 0; i < n; i++) {
        if (!strcmp(keys[i], "Name")) continue;
        char *v = g_key_file_get_value(scheme, "Scheme", keys[i], NULL);
        if (v) g_key_file_set_value(rc, "Configuration", keys[i], v);
        g_free(v);
    }
    g_strfreev(keys);

    g_key_file_set_boolean(rc, "Configuration", "ColorUseTheme", FALSE);
    if (g_key_file_has_key(rc, "Configuration", "ColorCursor", NULL))
        g_key_file_set_boolean(rc, "Configuration", "ColorCursorUseDefault", FALSE);
    if (g_key_file_has_key(rc, "Configuration", "ColorSelectionBackground", NULL) ||
        g_key_file_has_key(rc, "Configuration", "ColorSelection", NULL))
        g_key_file_set_boolean(rc, "Configuration", "ColorSelectionUseDefault", FALSE);
    if (g_key_file_has_key(rc, "Configuration", "ColorBold", NULL))
        g_key_file_set_boolean(rc, "Configuration", "ColorBoldUseDefault", FALSE);

    gsize len = 0;
    char *data = g_key_file_to_data(rc, &len, NULL);
    gboolean ok = g_file_set_contents(rcp, data, len, err);

    g_free(data); g_key_file_free(rc); g_key_file_free(scheme);
    g_free(rcp); g_free(dir); g_free(path);
    return ok;
}

static gboolean apply_mousepad(const char *id, GError **err)
{
    if (!mousepad_available()) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "schema do Mousepad (%s) não encontrado — o Mousepad está instalado?", MP_SCHEMA);
        return FALSE;
    }
    GSettings *s = g_settings_new(MP_SCHEMA);
    gboolean ok = g_settings_set_string(s, MP_KEY, id);
    g_settings_sync();
    g_object_unref(s);
    if (!ok)
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "não foi possível gravar o esquema do Mousepad");
    return ok;
}

/* ------------------------------------------------------------------ */
/* LightDM (lightdm-gtk-greeter) — exige root; usa pkexec              */
/* ------------------------------------------------------------------ */

/* o greeter roda como usuário "lightdm": só enxerga temas do sistema */
static gboolean sys_has(const char *sub, const char *name, const char *probe)
{
    for (const char * const *s = g_get_system_data_dirs(); *s; s++) {
        char *p = g_build_filename(*s, sub, name, probe, NULL);
        gboolean ok = g_file_test(p, G_FILE_TEST_EXISTS);
        g_free(p);
        if (ok) return TRUE;
    }
    return FALSE;
}

static GPtrArray *scan_gtk_system(void)
{
    GPtrArray *a = items_new();
    for (const char * const *s = g_get_system_data_dirs(); *s; s++) {
        char *d = g_build_filename(*s, "themes", NULL);
        GDir *g = g_dir_open(d, 0, NULL);
        if (g) {
            const char *n;
            while ((n = g_dir_read_name(g))) {
                char *p = g_build_filename(d, n, "gtk-3.0", NULL);
                if (g_file_test(p, G_FILE_TEST_IS_DIR)) items_add(a, n, n, NULL);
                g_free(p);
            }
            g_dir_close(g);
        }
        g_free(d);
    }
    g_ptr_array_sort(a, item_cmp);
    return a;
}

static gboolean safe_name(const char *n)
{
    if (!n || !*n || *n == '.' || strlen(n) > 128) return FALSE;
    for (const char *p = n; *p; p++)
        if (!(g_ascii_isalnum(*p) || strchr("._-+ ", *p))) return FALSE;
    return TRUE;
}

/* lê o tema/ícones atuais do greeter (o arquivo é legível por todos) */
static void lightdm_current(char **theme, char **icons)
{
    GKeyFile *k = g_key_file_new();
    *theme = NULL; *icons = NULL;
    if (g_key_file_load_from_file(k, LIGHTDM_CONF, 0, NULL)) {
        *theme = g_key_file_get_string(k, "greeter", "theme-name", NULL);
        *icons = g_key_file_get_string(k, "greeter", "icon-theme-name", NULL);
    }
    g_key_file_free(k);
}

/* modo root: --lightdm-write TEMA ÍCONES (strings vazias = não alterar) */
static int lightdm_write(const char *theme, const char *icons)
{
    if (geteuid() != 0) { g_printerr("precisa de privilégios de administrador\n"); return 3; }
    if (*theme && (!safe_name(theme) || !sys_has("themes", theme, "gtk-3.0"))) {
        g_printerr("tema GTK inválido ou não instalado no sistema: %s\n", theme); return 4;
    }
    if (*icons && (!safe_name(icons) || !sys_has("icons", icons, "index.theme"))) {
        g_printerr("tema de ícones inválido ou não instalado no sistema: %s\n", icons); return 4;
    }
    if (!g_file_test(LIGHTDM_DIR, G_FILE_TEST_IS_DIR)) {
        g_printerr("%s não existe — o LightDM está instalado?\n", LIGHTDM_DIR); return 5;
    }

    GKeyFile *k = g_key_file_new();
    g_key_file_load_from_file(k, LIGHTDM_CONF, G_KEY_FILE_KEEP_COMMENTS, NULL); /* pode não existir */

    /* backup único do arquivo original */
    char *bak = g_strconcat(LIGHTDM_CONF, ".bak-xfce-theme-profiles", NULL), *orig = NULL;
    if (!g_file_test(bak, G_FILE_TEST_EXISTS) && g_file_get_contents(LIGHTDM_CONF, &orig, NULL, NULL))
        g_file_set_contents(bak, orig, -1, NULL);
    g_free(orig); g_free(bak);

    if (*theme) g_key_file_set_string(k, "greeter", "theme-name", theme);
    if (*icons) g_key_file_set_string(k, "greeter", "icon-theme-name", icons);

    GError *e = NULL; gsize len;
    char *data = g_key_file_to_data(k, &len, NULL);
    gboolean ok = g_file_set_contents(LIGHTDM_CONF, data, len, &e);
    if (ok) chmod(LIGHTDM_CONF, 0644);
    else g_printerr("falha ao gravar %s: %s\n", LIGHTDM_CONF, e->message);
    g_clear_error(&e); g_free(data); g_key_file_free(k);
    return ok ? 0 : 6;
}

/* modo usuário: chama o próprio binário via pkexec */
static gboolean apply_lightdm(const char *theme, const char *icons, GError **err)
{
    char *self = g_file_read_link("/proc/self/exe", NULL);
    if (!self) self = g_strdup(APP_NAME);
    char *argv[] = { "pkexec", self, "--lightdm-write", (char *)theme, (char *)icons, NULL };
    char *errout = NULL; int status = 0;
    GError *e = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                      NULL, NULL, NULL, &errout, &status, &e)) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "não foi possível executar o pkexec (%s). Instale o polkit.", e->message);
        g_clear_error(&e); g_free(self); g_free(errout);
        return FALSE;
    }
    gboolean ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (code == 126 || code == 127)
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "autenticação de administrador cancelada ou negada");
        else
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                        errout && *g_strstrip(errout) ? errout : "falha ao gravar a configuração");
    }
    g_free(self); g_free(errout);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Armazenamento dos perfis                                            */
/* ------------------------------------------------------------------ */

static char *grp(const char *name) { return g_strconcat(PROFILE_PFX, name, NULL); }

static void cfg_load(void)
{
    kf = g_key_file_new();
    cfg_path = g_build_filename(g_get_user_config_dir(), APP_NAME, "profiles.ini", NULL);
    g_key_file_load_from_file(kf, cfg_path, G_KEY_FILE_NONE, NULL);
}

static void cfg_save(void)
{
    char *dir = g_path_get_dirname(cfg_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    gsize len; char *d = g_key_file_to_data(kf, &len, NULL);
    g_file_set_contents(cfg_path, d, len, NULL);
    g_free(d);
}

static gint str_cmp(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(*(char * const *)a, *(char * const *)b);
}

/* nomes de perfis ordenados; liberar com g_ptr_array_unref */
static GPtrArray *profile_names(void)
{
    GPtrArray *r = g_ptr_array_new_with_free_func(g_free);
    gsize n; char **g = g_key_file_get_groups(kf, &n);
    for (gsize i = 0; i < n; i++)
        if (g_str_has_prefix(g[i], PROFILE_PFX))
            g_ptr_array_add(r, g_strdup(g[i] + strlen(PROFILE_PFX)));
    g_strfreev(g);
    g_ptr_array_sort(r, str_cmp);
    return r;
}

static gboolean profile_exists(const char *name)
{
    char *g = grp(name); gboolean r = g_key_file_has_group(kf, g); g_free(g); return r;
}

static gboolean valid_name(const char *n)
{
    if (!n || !*n || strlen(n) > 64) return FALSE;
    for (const char *p = n; *p; p++)
        if (*p == '[' || *p == ']' || (guchar)*p < 0x20) return FALSE;
    return TRUE;
}

static char *unique_name(const char *base)
{
    if (!profile_exists(base)) return g_strdup(base);
    for (int i = 2;; i++) {
        char *c = g_strdup_printf("%s %d", base, i);
        if (!profile_exists(c)) return c;
        g_free(c);
    }
}

static char *active_profile(void)
{
    return g_key_file_get_string(kf, "General", "active", NULL);
}

static void rename_profile(const char *old, const char *neu)
{
    char *go = grp(old), *gn = grp(neu);
    gsize n; char **keys = g_key_file_get_keys(kf, go, &n, NULL);
    for (gsize i = 0; i < n; i++) {
        char *v = g_key_file_get_string(kf, go, keys[i], NULL);
        g_key_file_set_string(kf, gn, keys[i], v ? v : "");
        g_free(v);
    }
    g_strfreev(keys);
    g_key_file_remove_group(kf, go, NULL);
    char *act = active_profile();
    if (!g_strcmp0(act, old)) g_key_file_set_string(kf, "General", "active", neu);
    g_free(act); g_free(go); g_free(gn);
}

static void copy_profile(const char *src, const char *dst)
{
    char *gs = grp(src), *gd = grp(dst);
    gsize n; char **keys = g_key_file_get_keys(kf, gs, &n, NULL);
    for (gsize i = 0; i < n; i++) {
        char *v = g_key_file_get_string(kf, gs, keys[i], NULL);
        g_key_file_set_string(kf, gd, keys[i], v ? v : "");
        g_free(v);
    }
    g_strfreev(keys); g_free(gs); g_free(gd);
}

/* lê o estado atual do sistema */
static void capture_current(char **gtk, char **icons, char **term, char **mp,
                            char **xfwm, char **rofi)
{
    *gtk = xf_get("xsettings", "/Net/ThemeName");
    *icons = xf_get("xsettings", "/Net/IconThemeName");
    *xfwm = xf_get("xfwm4", "/general/theme");
    *rofi = capture_rofi();
    *mp = g_strdup("");
    *term = g_strdup("");

    if (mousepad_available()) {
        GSettings *s = g_settings_new(MP_SCHEMA);
        g_free(*mp);
        *mp = g_settings_get_string(s, MP_KEY);
        g_object_unref(s);
    }

    char *rcp = terminalrc_path();
    GKeyFile *rc = g_key_file_new();
    if (g_key_file_load_from_file(rc, rcp, 0, NULL)) {
        char *bg = g_key_file_get_string(rc, "Configuration", "ColorBackground", NULL);
        char *fg = g_key_file_get_string(rc, "Configuration", "ColorForeground", NULL);
        if (bg && fg) {
            GPtrArray *items = scan_terminal();
            for (guint i = 0; i < items->len; i++) {
                Item *it = items->pdata[i];
                GKeyFile *k = g_key_file_new();
                if (g_key_file_load_from_file(k, it->path, 0, NULL)) {
                    char *b = g_key_file_get_string(k, "Scheme", "ColorBackground", NULL);
                    char *f = g_key_file_get_string(k, "Scheme", "ColorForeground", NULL);
                    gboolean m = b && f && !g_ascii_strcasecmp(b, bg) && !g_ascii_strcasecmp(f, fg);
                    g_free(b); g_free(f);
                    if (m) { g_free(*term); *term = g_strdup(it->id); }
                }
                g_key_file_free(k);
                if (**term) break;
            }
            g_ptr_array_unref(items);
        }
        g_free(bg); g_free(fg);
    }
    g_key_file_free(rc); g_free(rcp);
}

/* aplica um perfil; acumula mensagens de erro em msg. TRUE se tudo ok */
static gboolean apply_profile(const char *name, GString *msg)
{
    char *g = grp(name);
    if (!g_key_file_has_group(kf, g)) {
        g_string_append_printf(msg, "Perfil '%s' não existe.\n", name);
        g_free(g);
        return FALSE;
    }
    gboolean all_ok = TRUE;
    struct { const char *key, *what; } steps[] = {
        { "gtk", "tema GTK" }, { "xfwm4", "tema do xfwm4" }, { "icons", "ícones" },
        { "terminal", "xfce4-terminal" }, { "mousepad", "Mousepad" }, { "rofi", "rofi" } };

    for (int i = 0; i < 6; i++) {
        char *v = g_key_file_get_string(kf, g, steps[i].key, NULL);
        if (v && *v) {
            GError *e = NULL; gboolean ok;
            switch (i) {
            case 0:  ok = xf_set("xsettings", "/Net/ThemeName", v, &e); break;
            case 1:  ok = xf_set("xfwm4", "/general/theme", v, &e); break;
            case 2:  ok = xf_set("xsettings", "/Net/IconThemeName", v, &e); break;
            case 3:  ok = apply_terminal(v, &e); break;
            case 4:  ok = apply_mousepad(v, &e); break;
            default: ok = apply_rofi(v, &e); break;
            }
            if (!ok) {
                g_string_append_printf(msg, "%s: %s\n", steps[i].what, e ? e->message : "erro");
                all_ok = FALSE;
            }
            g_clear_error(&e);
        }
        g_free(v);
    }

    /* LightDM (opcional): tema próprio do greeter e/ou reaproveitar GTK + ícones do perfil */
    {
        gboolean sync = g_key_file_get_boolean(kf, g, "lightdm_sync", NULL);
        char *ldt = g_key_file_get_string(kf, g, "lightdm", NULL);
        char *pg = g_key_file_get_string(kf, g, "gtk", NULL);
        char *pi = g_key_file_get_string(kf, g, "icons", NULL);
        char *t = g_strdup(ldt && *ldt ? ldt : (sync && pg ? pg : ""));
        char *ic = g_strdup(sync && pi ? pi : "");

        if (*t && !sys_has("themes", t, "gtk-3.0")) {
            g_string_append_printf(msg, "LightDM: o tema GTK '%s' não está instalado no sistema "
                "(ex.: /usr/share/themes); o greeter não lê temas do seu home.\n", t);
            all_ok = FALSE; *t = '\0';
        }
        if (*ic && !sys_has("icons", ic, "index.theme")) {
            g_string_append_printf(msg, "LightDM: o tema de ícones '%s' não está instalado no sistema "
                "(ex.: /usr/share/icons); o greeter não lê temas do seu home.\n", ic);
            all_ok = FALSE; *ic = '\0';
        }
        char *ct = NULL, *ci = NULL;
        lightdm_current(&ct, &ci);
        if (!g_strcmp0(t, ct)) *t = '\0';        /* já está igual: evita pedir senha à toa */
        if (!g_strcmp0(ic, ci)) *ic = '\0';
        if (*t || *ic) {
            GError *e = NULL;
            if (!apply_lightdm(t, ic, &e)) {
                g_string_append_printf(msg, "LightDM: %s\n", e ? e->message : "erro");
                all_ok = FALSE;
            }
            g_clear_error(&e);
        }
        g_free(ct); g_free(ci); g_free(t); g_free(ic); g_free(ldt); g_free(pg); g_free(pi);
    }
    g_key_file_set_string(kf, "General", "active", name);
    cfg_save();
    g_free(g);
    return all_ok;
}

/* ------------------------------------------------------------------ */
/* Interface gráfica                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    GtkWidget *win, *tree, *form, *e_name, *c_gtk, *c_xfwm, *c_icons, *c_term, *c_mp, *c_rofi, *c_ld, *chk_ld, *status;
    GtkListStore *store;
    GPtrArray *i_gtk, *i_xfwm, *i_icons, *i_term, *i_mp, *i_rofi, *i_ld;
    char *sel;
    gboolean loading, dirty;
} App;

static void set_status(App *a, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_status(App *a, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(a->status), s);
    g_free(s);
}

static void fill_combo(GtkWidget *cb, GPtrArray *items)
{
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cb), "", NONE_LABEL);
    for (guint i = 0; i < items->len; i++) {
        Item *it = items->pdata[i];
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cb), it->id, it->label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb), 0);
}

static void combo_set(GtkWidget *cb, const char *val)
{
    if (!val || !*val) { gtk_combo_box_set_active(GTK_COMBO_BOX(cb), 0); return; }
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(cb), val)) {
        char *l = g_strdup_printf("%s (não encontrado)", val);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cb), val, l);
        g_free(l);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(cb), val);
    }
}

static const char *combo_get(GtkWidget *cb)
{
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(cb));
    return id ? id : "";
}

static void refresh_list(App *a)
{
    GPtrArray *names = profile_names();
    char *act = active_profile();
    a->loading = TRUE;
    gtk_list_store_clear(a->store);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree));
    for (guint i = 0; i < names->len; i++) {
        GtkTreeIter it;
        const char *n = names->pdata[i];
        gtk_list_store_append(a->store, &it);
        gtk_list_store_set(a->store, &it, 0, n, 1, !g_strcmp0(n, act) ? "●" : "", -1);
        if (!g_strcmp0(n, a->sel)) gtk_tree_selection_select_iter(sel, &it);
    }
    a->loading = FALSE;
    g_free(act);
    g_ptr_array_unref(names);
}

static void load_form(App *a)
{
    a->loading = TRUE;
    gtk_widget_set_sensitive(a->form, a->sel != NULL);
    if (!a->sel) {
        gtk_entry_set_text(GTK_ENTRY(a->e_name), "");
        combo_set(a->c_gtk, ""); combo_set(a->c_xfwm, ""); combo_set(a->c_icons, "");
        combo_set(a->c_term, ""); combo_set(a->c_mp, ""); combo_set(a->c_rofi, "");
        combo_set(a->c_ld, "");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->chk_ld), FALSE);
    } else {
        char *g = grp(a->sel);
        gtk_entry_set_text(GTK_ENTRY(a->e_name), a->sel);
        struct { GtkWidget *cb; const char *key; } m[] = {
            { a->c_gtk, "gtk" }, { a->c_xfwm, "xfwm4" }, { a->c_icons, "icons" },
            { a->c_term, "terminal" }, { a->c_mp, "mousepad" }, { a->c_rofi, "rofi" } };
        for (int i = 0; i < 6; i++) {
            char *v = g_key_file_get_string(kf, g, m[i].key, NULL);
            combo_set(m[i].cb, v);
            g_free(v);
        }
        char *ld = g_key_file_get_string(kf, g, "lightdm", NULL);
        combo_set(a->c_ld, ld);
        g_free(ld);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->chk_ld),
                                     g_key_file_get_boolean(kf, g, "lightdm_sync", NULL));
        g_free(g);
    }
    a->dirty = FALSE;
    a->loading = FALSE;
}

/* seleciona um perfil (NULL = primeiro) e recarrega o formulário */
static void select_profile(App *a, const char *name)
{
    char *want = g_strdup(name);
    if (!want) {
        GPtrArray *n = profile_names();
        if (n->len) want = g_strdup(n->pdata[0]);
        g_ptr_array_unref(n);
    }
    g_free(a->sel);
    a->sel = want;
    refresh_list(a);
    load_form(a);
}

/* grava o formulário no perfil selecionado; TRUE se renomeou */
static gboolean commit_form(App *a)
{
    if (!a->sel) return FALSE;
    gboolean renamed = FALSE;
    char *g = grp(a->sel);
    g_key_file_set_string(kf, g, "gtk", combo_get(a->c_gtk));
    g_key_file_set_string(kf, g, "xfwm4", combo_get(a->c_xfwm));
    g_key_file_set_string(kf, g, "icons", combo_get(a->c_icons));
    g_key_file_set_string(kf, g, "terminal", combo_get(a->c_term));
    g_key_file_set_string(kf, g, "mousepad", combo_get(a->c_mp));
    g_key_file_set_string(kf, g, "rofi", combo_get(a->c_rofi));
    g_key_file_set_string(kf, g, "lightdm", combo_get(a->c_ld));
    g_key_file_set_boolean(kf, g, "lightdm_sync",
                           gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_ld)));
    g_free(g);

    char *neu = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(a->e_name))));
    if (strcmp(neu, a->sel) != 0) {
        if (!valid_name(neu))
            set_status(a, "Nome inválido; mantido '%s'.", a->sel);
        else if (profile_exists(neu))
            set_status(a, "Já existe um perfil '%s'; nome mantido.", neu);
        else {
            rename_profile(a->sel, neu);
            g_free(a->sel);
            a->sel = g_strdup(neu);
            renamed = TRUE;
        }
    }
    g_free(neu);
    cfg_save();
    a->dirty = FALSE;
    return renamed;
}

static void on_sel_changed(GtkTreeSelection *s, App *a)
{
    if (a->loading) return;
    gboolean refresh = a->dirty ? commit_form(a) : FALSE;
    GtkTreeIter it; GtkTreeModel *m; char *name = NULL;
    if (gtk_tree_selection_get_selected(s, &m, &it))
        gtk_tree_model_get(m, &it, 0, &name, -1);
    g_free(a->sel);
    a->sel = name;
    load_form(a);
    if (refresh) refresh_list(a);
}

static void on_dirty(GtkWidget *w, App *a) { (void)w; if (!a->loading) a->dirty = TRUE; }

static void on_save(GtkWidget *w, App *a)
{
    (void)w;
    if (!a->sel) return;
    if (commit_form(a)) refresh_list(a);
    set_status(a, "Perfil '%s' salvo.", a->sel);
}

static void on_apply(GtkWidget *w, App *a)
{
    (void)w;
    if (!a->sel) return;
    if (commit_form(a)) refresh_list(a);
    GString *msg = g_string_new(NULL);
    gboolean ok = apply_profile(a->sel, msg);
    refresh_list(a);
    if (ok) set_status(a, "Perfil '%s' aplicado.", a->sel);
    else    set_status(a, "Aplicado com avisos — %s", msg->str);
    g_string_free(msg, TRUE);
}

static void on_row_activated(GtkTreeView *t, GtkTreePath *p, GtkTreeViewColumn *c, App *a)
{
    (void)t; (void)p; (void)c;
    on_apply(NULL, a);
}

static void on_capture(GtkWidget *w, App *a)
{
    (void)w;
    char *gtk, *ic, *te, *mp, *xw, *ro;
    capture_current(&gtk, &ic, &te, &mp, &xw, &ro);
    combo_set(a->c_gtk, gtk); combo_set(a->c_xfwm, xw); combo_set(a->c_icons, ic);
    combo_set(a->c_term, te); combo_set(a->c_mp, mp); combo_set(a->c_rofi, ro);
    {
        char *lt = NULL, *li = NULL;
        lightdm_current(&lt, &li);
        combo_set(a->c_ld, lt);
        g_free(lt); g_free(li);
    }
    set_status(a, *te ? "Tema atual capturado (clique em Salvar)."
                      : "Tema atual capturado; o esquema do terminal não correspondeu a nenhum conhecido.");
    g_free(gtk); g_free(ic); g_free(te); g_free(mp); g_free(xw); g_free(ro);
}

static void on_new(GtkWidget *w, App *a)
{
    (void)w;
    if (a->dirty) commit_form(a);
    char *name = unique_name("Novo perfil"), *g = grp(name);
    char *gtk, *ic, *te, *mp, *xw, *ro;
    capture_current(&gtk, &ic, &te, &mp, &xw, &ro);
    g_key_file_set_string(kf, g, "gtk", gtk);
    g_key_file_set_string(kf, g, "xfwm4", xw);
    g_key_file_set_string(kf, g, "icons", ic);
    g_key_file_set_string(kf, g, "terminal", te);
    g_key_file_set_string(kf, g, "mousepad", mp);
    g_key_file_set_string(kf, g, "rofi", ro);
    cfg_save();
    select_profile(a, name);
    gtk_widget_grab_focus(a->e_name);
    gtk_editable_select_region(GTK_EDITABLE(a->e_name), 0, -1);
    g_free(gtk); g_free(ic); g_free(te); g_free(mp); g_free(xw); g_free(ro);
    g_free(g); g_free(name);
}

static void on_dup(GtkWidget *w, App *a)
{
    (void)w;
    if (!a->sel) return;
    if (a->dirty) commit_form(a);
    char *base = g_strdup_printf("%s (cópia)", a->sel);
    char *name = unique_name(base);
    copy_profile(a->sel, name);
    cfg_save();
    select_profile(a, name);
    g_free(base); g_free(name);
}

static void on_delete(GtkWidget *w, App *a)
{
    (void)w;
    if (!a->sel) return;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->win), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Excluir o perfil '%s'?", a->sel);
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancelar", GTK_RESPONSE_CANCEL,
                           "_Excluir", GTK_RESPONSE_ACCEPT, NULL);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r != GTK_RESPONSE_ACCEPT) return;

    char *g = grp(a->sel), *act = active_profile();
    g_key_file_remove_group(kf, g, NULL);
    if (!g_strcmp0(act, a->sel)) g_key_file_remove_key(kf, "General", "active", NULL);
    cfg_save();
    set_status(a, "Perfil '%s' excluído.", a->sel);
    g_free(g); g_free(act);
    select_profile(a, NULL);
}

static void grid_row(GtkWidget *grid, int r, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w, 1, r, 1, 1);
}

static GtkWidget *tool_button(const char *label, const char *icon, GCallback cb, App *a)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(b), TRUE);
    g_signal_connect(b, "clicked", cb, a);
    return b;
}

static void ensure_first_profile(void)
{
    GPtrArray *n = profile_names();
    if (n->len == 0) {
        char *gtk, *ic, *te, *mp, *xw, *ro;
        capture_current(&gtk, &ic, &te, &mp, &xw, &ro);
        char *g = grp("Atual");
        g_key_file_set_string(kf, g, "gtk", gtk);
        g_key_file_set_string(kf, g, "xfwm4", xw);
        g_key_file_set_string(kf, g, "icons", ic);
        g_key_file_set_string(kf, g, "terminal", te);
        g_key_file_set_string(kf, g, "mousepad", mp);
        g_key_file_set_string(kf, g, "rofi", ro);
        g_key_file_set_string(kf, "General", "active", "Atual");
        cfg_save();
        g_free(g); g_free(gtk); g_free(ic); g_free(te); g_free(mp); g_free(xw); g_free(ro);
    }
    g_ptr_array_unref(n);
}

static void build_ui(App *a)
{
    a->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a->win), "Perfis de Tema");
    gtk_window_set_default_size(GTK_WINDOW(a->win), 760, 540);
    gtk_window_set_icon_name(GTK_WINDOW(a->win), "preferences-desktop-theme");
    g_signal_connect(a->win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_container_add(GTK_CONTAINER(a->win), root);

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(root), hb, TRUE, TRUE, 0);

    /* lista de perfis */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(left, 220, -1);
    gtk_box_pack_start(GTK_BOX(hb), left, FALSE, FALSE, 0);

    a->store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    a->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(a->tree), FALSE);
    GtkCellRenderer *r1 = gtk_cell_renderer_text_new(), *r2 = gtk_cell_renderer_text_new();
    g_object_set(r1, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *c1 = gtk_tree_view_column_new_with_attributes("Perfil", r1, "text", 0, NULL);
    gtk_tree_view_column_set_expand(c1, TRUE);
    GtkTreeViewColumn *c2 = gtk_tree_view_column_new_with_attributes("Ativo", r2, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->tree), c1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->tree), c2);
    gtk_widget_set_tooltip_text(a->tree, "Duplo clique para aplicar. ● = perfil ativo");
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), a->tree);
    gtk_box_pack_start(GTK_BOX(left), sw, TRUE, TRUE, 0);

    GtkWidget *lb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(lb), tool_button("Novo", "list-add", G_CALLBACK(on_new), a), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(lb), tool_button("Duplicar", "edit-copy", G_CALLBACK(on_dup), a), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(lb), tool_button("Excluir", "list-remove", G_CALLBACK(on_delete), a), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(left), lb, FALSE, FALSE, 0);

    /* formulário */
    a->form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_pack_start(GTK_BOX(hb), a->form, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(a->form), grid, FALSE, FALSE, 0);

    a->e_name = gtk_entry_new();
    a->c_gtk = gtk_combo_box_text_new();
    a->c_xfwm = gtk_combo_box_text_new();
    a->c_icons = gtk_combo_box_text_new();
    a->c_term = gtk_combo_box_text_new();
    a->c_mp = gtk_combo_box_text_new();
    a->c_rofi = gtk_combo_box_text_new();
    fill_combo(a->c_gtk, a->i_gtk);
    fill_combo(a->c_xfwm, a->i_xfwm);
    fill_combo(a->c_icons, a->i_icons);
    fill_combo(a->c_term, a->i_term);
    fill_combo(a->c_mp, a->i_mp);
    fill_combo(a->c_rofi, a->i_rofi);
    a->c_ld = gtk_combo_box_text_new();
    fill_combo(a->c_ld, a->i_ld);
    a->chk_ld = gtk_check_button_new_with_label("Usar também o tema GTK e os ícones do perfil no LightDM");
    gtk_widget_set_tooltip_text(a->c_ld,
        "Tema GTK do greeter (apenas temas instalados no sistema, ex.: /usr/share/themes). "
        "Vazio: usa o tema GTK do perfil se a caixa abaixo estiver marcada.");
    gtk_widget_set_tooltip_text(a->chk_ld,
        "Opcional. Grava theme-name e icon-theme-name em " LIGHTDM_CONF
        " (pede senha de administrador via pkexec). Os temas precisam estar instalados no sistema.");
    if (!g_file_test(LIGHTDM_DIR, G_FILE_TEST_IS_DIR)) {
        gtk_widget_set_sensitive(a->c_ld, FALSE);
        gtk_widget_set_sensitive(a->chk_ld, FALSE);
        gtk_widget_set_tooltip_text(a->chk_ld, "LightDM não encontrado (/etc/lightdm ausente)");
    }
    if (!mousepad_available()) {
        gtk_widget_set_sensitive(a->c_mp, FALSE);
        gtk_widget_set_tooltip_text(a->c_mp, "Mousepad não encontrado (schema GSettings ausente)");
    }
    grid_row(grid, 0, "Nome do perfil", a->e_name);
    grid_row(grid, 1, "Tema GTK", a->c_gtk);
    grid_row(grid, 2, "Tema de janelas (xfwm4)", a->c_xfwm);
    grid_row(grid, 3, "Tema de ícones", a->c_icons);
    grid_row(grid, 4, "Terminal (xfce4-terminal)", a->c_term);
    grid_row(grid, 5, "Mousepad", a->c_mp);
    grid_row(grid, 6, "Rofi", a->c_rofi);
    grid_row(grid, 7, "Tema do LightDM", a->c_ld);
    gtk_grid_attach(GTK_GRID(grid), a->chk_ld, 1, 8, 1, 1);

    GtkWidget *hint = gtk_label_new("Campos em “(não alterar)” são ignorados ao aplicar o perfil.");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
    gtk_box_pack_start(GTK_BOX(a->form), hint, FALSE, FALSE, 0);

    GtkWidget *fb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(fb), FALSE);
    gtk_box_pack_start(GTK_BOX(fb), tool_button("Capturar tema atual", "document-revert", G_CALLBACK(on_capture), a), FALSE, FALSE, 0);
    GtkWidget *apply = tool_button("Aplicar perfil", "emblem-ok-symbolic", G_CALLBACK(on_apply), a);
    gtk_style_context_add_class(gtk_widget_get_style_context(apply), "suggested-action");
    gtk_box_pack_end(GTK_BOX(fb), apply, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(fb), tool_button("Salvar", "document-save", G_CALLBACK(on_save), a), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(a->form), fb, FALSE, FALSE, 0);

    a->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(a->status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(a->status), TRUE);
    gtk_box_pack_start(GTK_BOX(root), a->status, FALSE, FALSE, 0);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree)), "changed",
                     G_CALLBACK(on_sel_changed), a);
    g_signal_connect(a->tree, "row-activated", G_CALLBACK(on_row_activated), a);
    g_signal_connect(a->e_name, "changed", G_CALLBACK(on_dirty), a);
    GtkWidget *cbs[] = { a->c_gtk, a->c_xfwm, a->c_icons, a->c_term, a->c_mp, a->c_rofi };
    for (int i = 0; i < 6; i++) g_signal_connect(cbs[i], "changed", G_CALLBACK(on_dirty), a);
    g_signal_connect(a->c_ld, "changed", G_CALLBACK(on_dirty), a);
    g_signal_connect(a->chk_ld, "toggled", G_CALLBACK(on_dirty), a);
}

/* ------------------------------------------------------------------ */

static int cli(int argc, char **argv)
{
    const char *cmd = argv[1];
    if (!strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
        g_print("%s %s\n", APP_NAME, APP_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "--list") || !strcmp(cmd, "-l")) {
        GPtrArray *n = profile_names();
        char *act = active_profile();
        for (guint i = 0; i < n->len; i++)
            g_print("%s%s\n", !g_strcmp0(n->pdata[i], act) ? "* " : "  ", (char *)n->pdata[i]);
        g_free(act); g_ptr_array_unref(n);
        return 0;
    }
    if (!strcmp(cmd, "--active")) {
        char *act = active_profile();
        g_print("%s\n", act ? act : "");
        g_free(act);
        return 0;
    }
    if ((!strcmp(cmd, "--apply") || !strcmp(cmd, "-a")) && argc > 2) {
        GString *msg = g_string_new(NULL);
        gboolean ok = apply_profile(argv[2], msg);
        if (msg->len) g_printerr("%s", msg->str);
        else g_print("Perfil '%s' aplicado.\n", argv[2]);
        g_string_free(msg, TRUE);
        return ok ? 0 : 1;
    }
    g_print("Uso: %s [--list | --active | --apply NOME | --version]\n"
            "Sem argumentos abre a interface gráfica.\n", argv[0]);
    return !strcmp(cmd, "--help") || !strcmp(cmd, "-h") ? 0 : 2;
}

int main(int argc, char **argv)
{
    GError *e = NULL;
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        g_print("%s %s\n", APP_NAME, APP_VERSION);
        return 0;
    }
    /* modo interno (executado como root via pkexec) — sem xfconf/GTK */
    if (argc == 4 && !strcmp(argv[1], "--lightdm-write"))
        return lightdm_write(argv[2], argv[3]);
    xfconf_ok = xfconf_init(&e);
    if (!xfconf_ok) {
        g_printerr("Aviso: xfconf indisponível: %s\n", e ? e->message : "?");
        g_clear_error(&e);
    }
    cfg_load();

    int rc = 0;
    if (argc > 1) {
        rc = cli(argc, argv);
        if (xfconf_ok) xfconf_shutdown();
        return rc;
    }

    gtk_init(&argc, &argv);
    ensure_first_profile();

    App a = { 0 };
    a.i_gtk = scan_gtk();
    a.i_xfwm = scan_xfwm4();
    a.i_icons = scan_icons();
    a.i_term = scan_terminal();
    a.i_mp = scan_mousepad();
    a.i_rofi = scan_rofi();
    a.i_ld = scan_gtk_system();
    build_ui(&a);

    char *act = active_profile();
    select_profile(&a, act && profile_exists(act) ? act : NULL);
    g_free(act);

    gtk_widget_show_all(a.win);
    gtk_main();

    if (a.dirty) commit_form(&a);
    if (xfconf_ok) xfconf_shutdown();
    return rc;
}
