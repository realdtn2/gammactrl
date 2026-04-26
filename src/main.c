/*
 * gammactrl — gamma control for KDE Plasma Wayland
 * - Copies original ICC, patches only VCGT tag
 * - Fixed output detection, sync on first run
 * - Day 1 Persistence: Remembers original profile forever
 * - Added Reset All Settings & Relaunch functionality
 * - Added Change Day 1 Default functionality with forced Resync
 * - Per-Monitor Persistence: Independent baselines, multipliers, and ICCs
 * - Sync Cancel Fix: Seamlessly reverts to prior state on cancel
 * - Build: cmake -B build && cmake --build build
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#define ICC_DIR        "/.local/share/icc"
#define CONFIG_DIR     "/.config/gammactrl"
#define N_ENTRIES      256
#define MAX_OUTPUTS    16
#define GAMMA_MIN      -1.0
#define GAMMA_MAX       3.0
#define GAMMA_STEP      0.00001
#define SLIDER_DEFAULT  1.0

/* ── Logging macro ──────────────────────────────────────────────────────── */
#define LOG(fmt, ...) fprintf(stderr, "[gammactrl] " fmt "\n", ##__VA_ARGS__)

/* ── Per-Monitor Persistence ────────────────────────────────────────────── */

static double load_multiplier(const char *out_name) {
    const char *home = g_get_home_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/gammactrl/mult_%s", home, out_name);
    FILE *f = fopen(path, "r");
    if (!f) return SLIDER_DEFAULT;
    double v = SLIDER_DEFAULT;
    fscanf(f, "%lf", &v);
    fclose(f);
    if (v < GAMMA_MIN || v > GAMMA_MAX) return SLIDER_DEFAULT;
    return v;
}

static void save_multiplier(const char *out_name, double val) {
    const char *home = g_get_home_dir();
    char dir[512], path[512];
    snprintf(dir,  sizeof(dir),  "%s" CONFIG_DIR,  home);
    snprintf(path, sizeof(path), "%s/.config/gammactrl/mult_%s", home, out_name);
    mkdir(dir, 0755);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%.5f\n", val);
        fclose(f);
    }
}

static double load_base_gamma(const char *out_name) {
    const char *home = g_get_home_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/gammactrl/base_%s", home, out_name);
    LOG("Loading baseline from %s", path);
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG("No baseline file found for %s (first run).", out_name);
        return -1.0;
    }
    double v = -1.0;
    fscanf(f, "%lf", &v);
    fclose(f);
    if (v < 0.50 || v > 3.00) {
        LOG("Invalid baseline value %.2f, ignoring.", v);
        return -1.0;
    }
    LOG("Loaded baseline gamma = %.5f", v);
    return v;
}

static void save_base_gamma(const char *out_name, double gamma) {
    const char *home = g_get_home_dir();
    char dir[512], path[512];
    snprintf(dir,  sizeof(dir),  "%s" CONFIG_DIR,  home);
    snprintf(path, sizeof(path), "%s/.config/gammactrl/base_%s", home, out_name);
    mkdir(dir, 0755);
    FILE *f = fopen(path, "w");
    if (!f) {
        LOG("ERROR: Cannot save baseline to %s", path);
        return;
    }
    fprintf(f, "%.5f\n", gamma);
    fclose(f);
    LOG("Saved baseline gamma = %.5f to %s", gamma, path);
}

/* ── Utilities ──────────────────────────────────────────────────────────── */

static void write_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void write_be32(uint8_t *p, uint32_t v) {
    p[0]=v>>24; p[1]=(v>>16)&0xff; p[2]=(v>>8)&0xff; p[3]=v&0xff;
}
static void write_bes32(uint8_t *p, int32_t v) { write_be32(p, (uint32_t)v); }

/* ── Output list & Day 1 Persistence ────────────────────────────────────── */

typedef struct {
    char name[256];
    char icc[512];
    int  priority;
    int  icc_toggle;
} Output;

static int    g_n_outputs = 0;
static Output g_outputs[MAX_OUTPUTS];

static void manage_day1_profile(int cur) {
    const char *home = g_get_home_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/gammactrl/day1_%s", home, g_outputs[cur].name);

    FILE *f = fopen(path, "r");
    if (f) {
        char stored[512];
        if (fgets(stored, sizeof(stored), f)) {
            char *nl = strchr(stored, '\n');
            if (nl) *nl = '\0';

            if (strcmp(stored, "[NONE]") == 0) {
                g_outputs[cur].icc[0] = '\0';
            } else {
                strncpy(g_outputs[cur].icc, stored, sizeof(g_outputs[cur].icc)-1);
            }
            LOG("Output %d loaded persistent Day 1 profile: %s", cur, stored);
        }
        fclose(f);
    } else {
        if (strstr(g_outputs[cur].icc, "gammactrl_copy") || strstr(g_outputs[cur].icc, "gammactrl_sync")) {
            LOG("WARNING: Active profile is a temporary gammactrl file. Defaulting Day 1 to [NONE].");
            g_outputs[cur].icc[0] = '\0';
        }

        char dir[512];
        snprintf(dir, sizeof(dir), "%s" CONFIG_DIR, home);
        mkdir(dir, 0755);

        f = fopen(path, "w");
        if (f) {
            fprintf(f, "%s\n", g_outputs[cur].icc[0] ? g_outputs[cur].icc : "[NONE]");
            fclose(f);
            LOG("Saved Day 1 profile for output %d: %s", cur, g_outputs[cur].icc[0] ? g_outputs[cur].icc : "[NONE]");
        }
    }
}

static int detect_outputs(void) {
    LOG("Detecting outputs using kscreen-doctor...");
    g_n_outputs = 0;
    FILE *f = popen("kscreen-doctor -o 2>/dev/null", "r");
    if (!f) return 0;

    char line[1024];
    int cur = -1;
    while (fgets(line, sizeof(line), f)) {
        char *src = line, *dst = line;
        while (*src) {
            if (*src == '\033') {
                src++;
                if (*src == '[') {
                    src++;
                    while ((*src >= '0' && *src <= '9') || *src == ';') src++;
                    if (*src >= 'A' && *src <= 'z') src++;
                }
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';

        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *output_pos = strstr(line, "Output:");
        if (output_pos) {
            char *p = output_pos + 7;
            while (*p == ' ') p++;

            int id = 1;
            char name[256] = {0};

            if (sscanf(p, "%d %255s", &id, name) == 2 || sscanf(p, "%255s", name) == 1) {
                if (g_n_outputs < MAX_OUTPUTS) {
                    cur = g_n_outputs++;
                    strncpy(g_outputs[cur].name, name, sizeof(g_outputs[cur].name)-1);
                    g_outputs[cur].icc[0] = '\0';
                    g_outputs[cur].priority = id;
                }
            }
        }

        if (cur >= 0 && strstr(line, "ICC profile:")) {
            char *colon = strchr(line, ':');
            if (colon) {
                char *val = colon + 1;
                while (*val == ' ') val++;
                char *end = val + strlen(val) - 1;
                while (end > val && (*end == ' ' || *end == '\t' || *end == '\r')) end--;
                *(end+1) = '\0';

                if (strcmp(val, "none") != 0 && strlen(val) > 0) {
                    strncpy(g_outputs[cur].icc, val, sizeof(g_outputs[cur].icc)-1);
                }
            }
        }
    }
    pclose(f);

    for (int i = 0; i < g_n_outputs; i++) {
        manage_day1_profile(i);
    }

    LOG("Detected %d output(s).", g_n_outputs);
    return g_n_outputs;
}

static void apply_icc(const char *output_name, const char *icc_path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "kscreen-doctor output.%s.iccprofile.\"%s\"", output_name, icc_path);
    LOG("Applying ICC: %s", cmd);
    system(cmd);
}

/* ── ICC patching ───────────────────────────────────────────────────────── */

static uint8_t *build_vcgt_tag(double gamma, size_t *out_len) {
    size_t data_len = 4+4+4+2+2+2 + (3 * N_ENTRIES * 2);
    uint8_t *tag = calloc(1, data_len);
    if (!tag) return NULL;
    *out_len = data_len;
    uint8_t *p = tag;
    memcpy(p, "vcgt", 4); p += 4;
    write_be32(p, 0); p += 4;
    write_be32(p, 0); p += 4;
    write_be16(p, 3); p += 2;
    write_be16(p, N_ENTRIES); p += 2;
    write_be16(p, 2); p += 2;
    for (int ch = 0; ch < 3; ch++) {
        for (int i = 0; i < N_ENTRIES; i++) {
            double v = pow((double)i / (N_ENTRIES-1), 1.0 / gamma);
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            write_be16(p, (uint16_t)(v * 65535));
            p += 2;
        }
    }
    return tag;
}

static int copy_and_patch_icc(const char *src_path, const char *dst_path, double gamma) {
    FILE *src = fopen(src_path, "rb");
    if (!src) return -1;
    fseek(src, 0, SEEK_END);
    long file_size = ftell(src);
    fseek(src, 0, SEEK_SET);
    uint8_t *data = malloc(file_size);
    if (!data) { fclose(src); return -1; }
    size_t read = fread(data, 1, file_size, src);
    fclose(src);
    if (read != (size_t)file_size || file_size < 128) { free(data); return -1; }

    uint32_t profile_size = (data[0]<<24)|(data[1]<<16)|(data[2]<<8)|data[3];
    if (profile_size != file_size) { free(data); return -1; }
    uint32_t tag_count = (data[128]<<24)|(data[129]<<16)|(data[130]<<8)|data[131];
    if (tag_count == 0 || tag_count > 100) { free(data); return -1; }

    uint8_t *tag_table = data + 128 + 4;
    int vcgt_index = -1;
    uint32_t vcgt_offset = 0, vcgt_size = 0;
    for (uint32_t i = 0; i < tag_count; i++) {
        uint8_t *entry = tag_table + i * 12;
        if (memcmp(entry, "vcgt", 4) == 0) {
            vcgt_index = i;
            vcgt_offset = (entry[4]<<24)|(entry[5]<<16)|(entry[6]<<8)|entry[7];
            vcgt_size   = (entry[8]<<24)|(entry[9]<<16)|(entry[10]<<8)|entry[11];
            break;
        }
    }
    if (vcgt_index == -1) { free(data); return -2; }

    size_t new_len;
    uint8_t *new_vcgt = build_vcgt_tag(gamma, &new_len);
    if (!new_vcgt || new_len > vcgt_size) {
        free(new_vcgt);
        free(data);
        return -2;
    }

    uint8_t *copy = malloc(file_size);
    if (!copy) { free(new_vcgt); free(data); return -1; }
    memcpy(copy, data, file_size);
    memcpy(copy + vcgt_offset, new_vcgt, new_len);
    free(new_vcgt);
    free(data);

    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { free(copy); return -1; }
    fwrite(copy, 1, file_size, dst);
    fclose(dst);
    free(copy);
    return 0;
}

static uint8_t *build_icc_from_scratch(double actual_gamma, size_t *out_sz) {
    #define MLUC_TEXT     "gammactrl"
    #define MLUC_TEXT_LEN 9
    #define MLUC_FULL     (4+4+4+4+4+4+4+MLUC_TEXT_LEN*2)
    #define XYZ_SIZE      (4+4+4+4+4)
    #define PARA_SIZE     (4+4+2+2+4)
    #define VCGT_HDR      (4+4+4+2+2+2)
    #define VCGT_DATA     (N_ENTRIES*2*3)
    #define VCGT_SIZE     (VCGT_HDR+VCGT_DATA)
    #define PAD4(x)       (((x)+3)&~3)
    #define MLUC_PAD      PAD4(MLUC_FULL)
    #define XYZ_PAD       PAD4(XYZ_SIZE)
    #define PARA_PAD      PAD4(PARA_SIZE)
    #define VCGT_PAD      PAD4(VCGT_SIZE)

    int    nt  = 10;
    size_t ds  = 128 + 4 + 12 * nt;
    size_t oc  = ds;
    size_t od  = oc + MLUC_PAD;
    size_t ow  = od + MLUC_PAD;
    size_t orx = ow  + XYZ_PAD;
    size_t ogx = orx + XYZ_PAD;
    size_t obx = ogx + XYZ_PAD;
    size_t ort = obx + XYZ_PAD;
    size_t ogt = ort + PARA_PAD;
    size_t obt = ogt + PARA_PAD;
    size_t ov  = obt + PARA_PAD;
    size_t tot = ov  + VCGT_PAD;

    uint8_t *b = calloc(1, tot);
    if (!b) return NULL;
    *out_sz = tot;

    write_be32(b,    (uint32_t)tot);
    memset(b+4,' ',4);
    write_be32(b+8,  0x02100000);
    memcpy(b+12,"mntr",4); memcpy(b+16,"RGB ",4); memcpy(b+20,"XYZ ",4);
    time_t t = time(NULL); struct tm *tm = gmtime(&t);
    write_be16(b+24,(uint16_t)(tm->tm_year+1900));
    write_be16(b+26,(uint16_t)(tm->tm_mon+1));
    write_be16(b+28,(uint16_t)tm->tm_mday);
    write_be16(b+30,(uint16_t)tm->tm_hour);
    write_be16(b+32,(uint16_t)tm->tm_min);
    write_be16(b+34,(uint16_t)tm->tm_sec);
    memcpy(b+36,"acsp",4); memcpy(b+40,"APPL",4);
    write_bes32(b+72,63190); write_bes32(b+76,65536); write_bes32(b+80,54061);

    uint8_t *tt = b+128;
    write_be32(tt,(uint32_t)nt); tt+=4;
    #define WT(sig,o,s) do{ memcpy(tt,sig,4);tt+=4; write_be32(tt,(uint32_t)(o));tt+=4; write_be32(tt,(uint32_t)(s));tt+=4; }while(0)
    WT("cprt",oc,MLUC_FULL); WT("desc",od,MLUC_FULL);
    WT("wtpt",ow,XYZ_SIZE);  WT("rXYZ",orx,XYZ_SIZE);
    WT("gXYZ",ogx,XYZ_SIZE); WT("bXYZ",obx,XYZ_SIZE);
    WT("rTRC",ort,PARA_SIZE); WT("gTRC",ogt,PARA_SIZE);
    WT("bTRC",obt,PARA_SIZE); WT("vcgt",ov,VCGT_SIZE);

    #define WM(o) do{ uint8_t *p=b+(o); memcpy(p,"mluc",4);p+=4;p+=4; write_be32(p,1);p+=4;write_be32(p,12);p+=4; memcpy(p,"enUS",4);p+=4; write_be32(p,MLUC_TEXT_LEN*2);p+=4; write_be32(p,28);p+=4; for(int _i=0;_i<MLUC_TEXT_LEN;_i++){p[_i*2]=0;p[_i*2+1]=MLUC_TEXT[_i];} }while(0)
    WM(oc); WM(od);

    #define WX(o,x,y,z) do{ uint8_t *p=b+(o);memcpy(p,"XYZ ",4);p+=4;p+=4; write_bes32(p,(int32_t)((x)*65536));p+=4; write_bes32(p,(int32_t)((y)*65536));p+=4; write_bes32(p,(int32_t)((z)*65536)); }while(0)
    WX(ow, 0.95045,1.00000,1.08905);
    WX(orx,0.43607,0.22249,0.01392);
    WX(ogx,0.38515,0.71687,0.09708);
    WX(obx,0.14307,0.06061,0.71393);

    #define WP(o,g) do{ uint8_t *p=b+(o);memcpy(p,"para",4);p+=4;p+=4; write_be16(p,0);p+=2;write_be16(p,0);p+=2; write_bes32(p,(int32_t)((g)*65536)); }while(0)
    WP(ort,actual_gamma); WP(ogt,actual_gamma); WP(obt,actual_gamma);

    uint8_t *p = b+ov;
    memcpy(p,"vcgt",4); p+=4; p+=4;
    write_be32(p,0); p+=4;
    write_be16(p,3); p+=2;
    write_be16(p,N_ENTRIES); p+=2;
    write_be16(p,2); p+=2;
    for (int ch = 0; ch < 3; ch++) {
        for (int i = 0; i < N_ENTRIES; i++) {
            double v = pow((double)i / (N_ENTRIES-1), 1.0 / actual_gamma);
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            write_be16(p, (uint16_t)(v * 65535));
            p += 2;
        }
    }
    #undef WT
    #undef WM
    #undef WX
    #undef WP
    return b;
}

static int write_icc_from_scratch(const char *path, double actual_gamma) {
    size_t sz;
    uint8_t *buf = build_icc_from_scratch(actual_gamma, &sz);
    if (!buf) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(buf, 1, sz, f);
    fclose(f);
    free(buf);
    return 0;
}

/* ── App state ──────────────────────────────────────────────────────────── */
typedef struct {
    GtkWidget *monitor_combo;
    GtkWidget *scale;
    GtkWidget *spin;
    GtkWidget *value_label;
    GtkWidget *status_label;
    GtkWidget *main_window;
    int        cur_output;
    gboolean   updating;
    double     base_gamma;
    gboolean   baseline_valid;
} AppWidgets;

static void show_sync_dialog(AppWidgets *w);
static void do_apply(AppWidgets *w);

/* ── Main apply function ────────────────────────────────────────────────── */
static int apply_gamma(AppWidgets *w, double actual_gamma) {
    const char *out = g_outputs[w->cur_output].name;
    const char *orig_icc = g_outputs[w->cur_output].icc;
    const char *home = g_get_home_dir();

    g_outputs[w->cur_output].icc_toggle ^= 1;
    int toggle = g_outputs[w->cur_output].icc_toggle;
    char active[512], stale[512];

    // Generate Per-Monitor Copy files to prevent display cross-contamination
    if (toggle) {
        snprintf(active, sizeof(active), "%s/.local/share/icc/gammactrl_copy_%s_2.icc", home, out);
        snprintf(stale,  sizeof(stale),  "%s/.local/share/icc/gammactrl_copy_%s_1.icc", home, out);
    } else {
        snprintf(active, sizeof(active), "%s/.local/share/icc/gammactrl_copy_%s_1.icc", home, out);
        snprintf(stale,  sizeof(stale),  "%s/.local/share/icc/gammactrl_copy_%s_2.icc", home, out);
    }
    remove(stale);

    if (orig_icc[0] != '\0' && access(orig_icc, R_OK) == 0) {
        int ret = copy_and_patch_icc(orig_icc, active, actual_gamma);
        if (ret == 0) {
            apply_icc(out, active);
            return 0;
        }
    }
    if (write_icc_from_scratch(active, actual_gamma) != 0) return -1;
    apply_icc(out, active);
    return 0;
}

/* ── Callbacks ─────────────────────────────────────────────────────────── */
static void do_apply(AppWidgets *w) {
    if (!w->baseline_valid) return;
    double slider_val = gtk_range_get_value(GTK_RANGE(w->scale));

    save_multiplier(g_outputs[w->cur_output].name, slider_val);

    double actual_gamma = pow(w->base_gamma, slider_val);

    if (apply_gamma(w, actual_gamma) != 0) {
        gtk_label_set_text(GTK_LABEL(w->status_label), "Error applying gamma");
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Applied — multiplier %.5f → gamma %.5f", slider_val, actual_gamma);
    gtk_label_set_text(GTK_LABEL(w->status_label), msg);
}

static guint debounce_id = 0;
static gboolean debounce_cb(gpointer d) {
    debounce_id = 0;
    do_apply((AppWidgets*)d);
    return G_SOURCE_REMOVE;
}
static void schedule_apply(AppWidgets *w) {
    if (debounce_id) g_source_remove(debounce_id);
    debounce_id = g_timeout_add(200, debounce_cb, w);
}

static void sync_value_label(AppWidgets *w, double slider_val) {
    char buf[16]; snprintf(buf, sizeof(buf), "%.5f", slider_val);
    gtk_label_set_text(GTK_LABEL(w->value_label), buf);
}
static void on_scale_changed(GtkRange *scale, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    if (w->updating) return;
    double val = gtk_range_get_value(scale);
    w->updating = TRUE;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->spin), val);
    w->updating = FALSE;
    sync_value_label(w, val);
    schedule_apply(w);
}
static void on_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    if (w->updating) return;
    double val = gtk_spin_button_get_value(spin);
    w->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(w->scale), val);
    w->updating = FALSE;
    sync_value_label(w, val);
    schedule_apply(w);
}

/* Dynamic monitor switching */
static void on_monitor_changed(GtkDropDown *dd, GParamSpec *ps, gpointer user_data) {
    (void)ps;
    AppWidgets *w = (AppWidgets*)user_data;
    w->cur_output = (int)gtk_drop_down_get_selected(dd);
    const char *out_name = g_outputs[w->cur_output].name;

    // Load independent state for the new monitor
    double saved_base = load_base_gamma(out_name);
    w->base_gamma = (saved_base > 0) ? saved_base : 1.485;
    w->baseline_valid = TRUE;

    double saved_mult = load_multiplier(out_name);

    // Smoothly update UI without triggering a debounce storm
    w->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(w->scale), saved_mult);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->spin), saved_mult);
    w->updating = FALSE;
    sync_value_label(w, saved_mult);

    gtk_scale_clear_marks(GTK_SCALE(w->scale));

    if (w->baseline_valid) {
        char mark_label[48]; snprintf(mark_label, sizeof(mark_label), "1.0 (baseline %.5f)", w->base_gamma);
        gtk_scale_add_mark(GTK_SCALE(w->scale), 1.0, GTK_POS_RIGHT, mark_label);
        gtk_label_set_text(GTK_LABEL(w->status_label), "Adjust multiplier (1.0 = baseline).");
        do_apply(w);
    }
}

static void on_reset(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *w = (AppWidgets*)user_data;
    const char *out = g_outputs[w->cur_output].name;
    const char *orig_icc = g_outputs[w->cur_output].icc;

    if (orig_icc[0] != '\0' && access(orig_icc, R_OK) == 0) {
        apply_icc(out, orig_icc);
    } else {
        apply_icc(out, "");
    }

    w->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(w->scale), SLIDER_DEFAULT);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->spin), SLIDER_DEFAULT);
    w->updating = FALSE;
    sync_value_label(w, SLIDER_DEFAULT);
    save_multiplier(out, SLIDER_DEFAULT);

    char msg[128];
    snprintf(msg, sizeof(msg), "Reset to %s", (orig_icc[0] ? "Default Profile" : "Native Output"));
    gtk_label_set_text(GTK_LABEL(w->status_label), msg);
}

/* ── Custom Day 1 Default Logic ─────────────────────────────────────────── */

static void apply_default_change(AppWidgets *w, GtkWidget *parent_dlg, const char *new_icc) {
    int cur = w->cur_output;
    const char *home = g_get_home_dir();
    char path[512];

    snprintf(path, sizeof(path), "%s/.config/gammactrl/day1_%s", home, g_outputs[cur].name);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", (new_icc && new_icc[0]) ? new_icc : "[NONE]");
        fclose(f);
    }

    if (new_icc && new_icc[0]) {
        strncpy(g_outputs[cur].icc, new_icc, sizeof(g_outputs[cur].icc)-1);
    } else {
        g_outputs[cur].icc[0] = '\0';
    }

    apply_icc(g_outputs[cur].name, g_outputs[cur].icc);

    w->base_gamma = 1.485;
    w->baseline_valid = TRUE;

    w->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(w->scale), SLIDER_DEFAULT);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->spin), SLIDER_DEFAULT);
    w->updating = FALSE;
    sync_value_label(w, SLIDER_DEFAULT);
    save_multiplier(g_outputs[cur].name, SLIDER_DEFAULT);

    gtk_label_set_text(GTK_LABEL(w->status_label), "Default ICC profile updated.");
    gtk_scale_clear_marks(GTK_SCALE(w->scale));
    char mark_label[48]; snprintf(mark_label, sizeof(mark_label), "1.0 (baseline %.5f)", w->base_gamma);
    gtk_scale_add_mark(GTK_SCALE(w->scale), 1.0, GTK_POS_RIGHT, mark_label);

    if (parent_dlg) gtk_window_destroy(GTK_WINDOW(parent_dlg));
}

static void on_native_day1_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    GtkWidget *dlg = g_object_get_data(G_OBJECT(btn), "dialog");
    apply_default_change(w, dlg, "");
}

static void on_file_dialog_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, result, &error);
    if (file) {
        char *filename = g_file_get_path(file);
        apply_default_change(w, NULL, filename);
        g_free(filename);
        g_object_unref(file);
    }
    if (error) g_error_free(error);
}

static void on_file_day1_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    GtkWidget *dlg = g_object_get_data(G_OBJECT(btn), "dialog");

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select ICC Profile");
    gtk_file_dialog_open(dialog, GTK_WINDOW(w->main_window), NULL,
                         on_file_dialog_response, w);
    g_object_unref(dialog);

    gtk_window_destroy(GTK_WINDOW(dlg));
}

static void on_change_default_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *w = (AppWidgets*)user_data;

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Change Default");
    gtk_window_set_default_size(GTK_WINDOW(dlg), 300, 150);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(w->main_window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);
    gtk_window_set_child(GTK_WINDOW(dlg), box);

    char msg[256];
    snprintf(msg, sizeof(msg), "Update the default profile for %s:", g_outputs[w->cur_output].name);
    GtkWidget *lbl = gtk_label_new(msg);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_box_append(GTK_BOX(box), lbl);

    GtkWidget *btn_icc = gtk_button_new_with_label("Select ICC File...");
    g_object_set_data(G_OBJECT(btn_icc), "dialog", dlg);
    g_signal_connect(btn_icc, "clicked", G_CALLBACK(on_file_day1_clicked), w);
    gtk_box_append(GTK_BOX(box), btn_icc);

    GtkWidget *btn_native = gtk_button_new_with_label("Set to Native (No Profile)");
    g_object_set_data(G_OBJECT(btn_native), "dialog", dlg);
    g_signal_connect(btn_native, "clicked", G_CALLBACK(on_native_day1_clicked), w);
    gtk_box_append(GTK_BOX(box), btn_native);

    gtk_window_present(GTK_WINDOW(dlg));
}

/* ── Reset All and Relaunch ────────────────────────────────────────────── */
static void on_reset_all(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    LOG("Reset All Settings & Relaunch clicked");

    const char *home = g_get_home_dir();
    char path[1024];

    // Wipe configurations for all detected screens
    for (int i = 0; i < g_n_outputs; i++) {
        const char *out = g_outputs[i].name;
        const char *orig_icc = g_outputs[i].icc;

        // Restore screen to its Day 1 state
        if (orig_icc[0] != '\0' && access(orig_icc, R_OK) == 0) {
            apply_icc(out, orig_icc);
        } else {
            apply_icc(out, "");
        }

        // Delete per-monitor config and scratch files
        snprintf(path, sizeof(path), "%s/.local/share/icc/gammactrl_copy_%s_1.icc", home, out);
        remove(path);
        snprintf(path, sizeof(path), "%s/.local/share/icc/gammactrl_copy_%s_2.icc", home, out);
        remove(path);
        snprintf(path, sizeof(path), "%s/.local/share/icc/gammactrl_sync_%s.icc", home, out);
        remove(path);

        snprintf(path, sizeof(path), "%s/.config/gammactrl/base_%s", home, out);
        remove(path);
        snprintf(path, sizeof(path), "%s/.config/gammactrl/mult_%s", home, out);
        remove(path);
        snprintf(path, sizeof(path), "%s/.config/gammactrl/day1_%s", home, out);
        remove(path);
    }

    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        LOG("Relaunching: %s", exe_path);
        /* Remove lockfile before exec so the new instance can acquire it.
         *          O_CLOEXEC on the fd handles this too, but deleting is belt-and-suspenders. */
        const char *home2 = g_get_home_dir();
        char lock_path[512];
        snprintf(lock_path, sizeof(lock_path), "%s/.config/gammactrl/gammactrl.lock", home2);
        remove(lock_path);
        char *args[] = { exe_path, NULL };
        execv(exe_path, args);
    }
}

/* ── Sync dialog ────────────────────────────────────────────────────────── */
typedef struct {
    AppWidgets *w;
    GtkWidget  *dialog;
    GtkWidget  *scale;
    GtkWidget  *spin;
    GtkWidget  *value_label;
    GtkWidget  *status_label;
    gboolean    updating;
    char        orig_icc[512];
    guint       toggle_timer_id;
    int         toggle_phase;
    char        preview_icc[512];
} SyncWidgets;

static void sync_apply_new(SyncWidgets *s) {
    double gamma = gtk_range_get_value(GTK_RANGE(s->scale));
    const char *out = g_outputs[s->w->cur_output].name;
    write_icc_from_scratch(s->preview_icc, gamma);
    apply_icc(out, s->preview_icc);
}
static void sync_restore_orig(SyncWidgets *s) {
    const char *out = g_outputs[s->w->cur_output].name;
    apply_icc(out, s->orig_icc);
}
static gboolean sync_toggle_cb(gpointer user_data) {
    SyncWidgets *s = (SyncWidgets*)user_data;
    if (!GTK_IS_WIDGET(s->dialog)) return G_SOURCE_REMOVE;
    s->toggle_phase ^= 1;
    if (s->toggle_phase == 0) {
        sync_restore_orig(s);
        gtk_label_set_text(GTK_LABEL(s->status_label), "◀ Original State (before calibration)");
    } else {
        sync_apply_new(s);
        double gamma = gtk_range_get_value(GTK_RANGE(s->scale));
        char msg[64]; snprintf(msg, sizeof(msg), "▶ New gamma: %.5f", gamma);
        gtk_label_set_text(GTK_LABEL(s->status_label), msg);
    }
    return G_SOURCE_CONTINUE;
}
static void sync_restart_timer(SyncWidgets *s) {
    if (s->toggle_timer_id) g_source_remove(s->toggle_timer_id);
    s->toggle_phase = 1;
    sync_apply_new(s);
    double gamma = gtk_range_get_value(GTK_RANGE(s->scale));
    char msg[64]; snprintf(msg, sizeof(msg), "▶ New gamma: %.5f", gamma);
    gtk_label_set_text(GTK_LABEL(s->status_label), msg);
    s->toggle_timer_id = g_timeout_add(1000, sync_toggle_cb, s);
}
static void on_sync_scale_changed(GtkRange *scale, gpointer user_data) {
    SyncWidgets *s = (SyncWidgets*)user_data;
    if (s->updating) return;
    double gamma = gtk_range_get_value(scale);
    s->updating = TRUE;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->spin), gamma);
    s->updating = FALSE;
    char buf[16]; snprintf(buf, sizeof(buf), "%.5f", gamma);
    gtk_label_set_text(GTK_LABEL(s->value_label), buf);
    sync_restart_timer(s);
}
static void on_sync_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    SyncWidgets *s = (SyncWidgets*)user_data;
    if (s->updating) return;
    double gamma = gtk_spin_button_get_value(spin);
    s->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(s->scale), gamma);
    s->updating = FALSE;
    char buf[16]; snprintf(buf, sizeof(buf), "%.5f", gamma);
    gtk_label_set_text(GTK_LABEL(s->value_label), buf);
    sync_restart_timer(s);
}
static void sync_cleanup(SyncWidgets *s) {
    if (s->toggle_timer_id) g_source_remove(s->toggle_timer_id);
    s->toggle_timer_id = 0;
    if (s->preview_icc[0]) remove(s->preview_icc);
}

static void on_sync_confirm(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SyncWidgets *s = (SyncWidgets*)user_data;
    double chosen_gamma = gtk_range_get_value(GTK_RANGE(s->scale));
    sync_cleanup(s);

    const char *out_name = g_outputs[s->w->cur_output].name;
    save_base_gamma(out_name, chosen_gamma);

    s->w->base_gamma = chosen_gamma;
    s->w->baseline_valid = TRUE;

    if (s->w->scale) {
        char mark_label[48]; snprintf(mark_label, sizeof(mark_label), "1.0 (baseline %.5f)", chosen_gamma);
        gtk_scale_clear_marks(GTK_SCALE(s->w->scale));
        gtk_scale_add_mark(GTK_SCALE(s->w->scale), SLIDER_DEFAULT, GTK_POS_RIGHT, mark_label);
        s->w->updating = TRUE;
        gtk_range_set_value(GTK_RANGE(s->w->scale), SLIDER_DEFAULT);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->w->spin), SLIDER_DEFAULT);
        s->w->updating = FALSE;
        sync_value_label(s->w, SLIDER_DEFAULT);
        save_multiplier(out_name, SLIDER_DEFAULT);
    }

    do_apply(s->w);

    char msg[128]; snprintf(msg, sizeof(msg), "Baseline saved and applied: gamma = %.5f.", chosen_gamma);
    gtk_label_set_text(GTK_LABEL(s->w->status_label), msg);
    gtk_window_destroy(GTK_WINDOW(s->dialog));
}

static void on_sync_cancel(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SyncWidgets *s = (SyncWidgets*)user_data;
    sync_cleanup(s);

    if (s->w->baseline_valid) {
        do_apply(s->w);
    } else {
        sync_restore_orig(s);
    }

    gtk_window_destroy(GTK_WINDOW(s->dialog));
}

static gboolean on_sync_close_request(GtkWindow *win, gpointer user_data) {
    (void)win;
    SyncWidgets *s = (SyncWidgets*)user_data;
    sync_cleanup(s);

    if (s->w->baseline_valid) {
        do_apply(s->w);
    } else {
        sync_restore_orig(s);
    }

    return FALSE;
}

static void show_sync_dialog(AppWidgets *w) {
    SyncWidgets *s = g_new0(SyncWidgets, 1);
    s->w = w;

    const char *out_name = g_outputs[w->cur_output].name;
    strncpy(s->orig_icc, g_outputs[w->cur_output].icc, sizeof(s->orig_icc)-1);

    const char *home = g_get_home_dir();
    snprintf(s->preview_icc, sizeof(s->preview_icc), "%s/.local/share/icc/gammactrl_sync_%s.icc", home, out_name);

    s->toggle_phase = 0;
    sync_restore_orig(s);

    GtkWidget *dlg = gtk_window_new();
    s->dialog = dlg;

    char title[128];
    snprintf(title, sizeof(title), "Sync Baseline - %s", out_name);
    gtk_window_set_title(GTK_WINDOW(dlg), title);

    gtk_window_set_default_size(GTK_WINDOW(dlg), 290, 580);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    if (w->main_window) gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(w->main_window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);
    gtk_widget_set_margin_top(root, 20);
    gtk_widget_set_margin_bottom(root, 20);
    gtk_window_set_child(GTK_WINDOW(dlg), root);

    GtkWidget *heading = gtk_label_new("Sync Baseline");
    gtk_widget_add_css_class(heading, "title-2");
    gtk_widget_set_halign(heading, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), heading);

    const char *orig_desc = (s->orig_icc[0] == '\0') ? "no profile (native output)" : s->orig_icc;
    char inst_text[700];
    snprintf(inst_text, sizeof(inst_text),
             "Your screen will alternate every second between your original state and the new gamma.\n\n"
             "Original State: %s\n\n"
             "Drag the slider to find the gamma that looks correct. When satisfied, click Confirm.",
             orig_desc);
    GtkWidget *inst = gtk_label_new(inst_text);
    gtk_label_set_wrap(GTK_LABEL(inst), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(inst), 36);
    gtk_widget_add_css_class(inst, "dim-label");
    gtk_box_append(GTK_BOX(root), inst);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_vexpand(row, TRUE);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), row);

    GtkWidget *lbl_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *lbl_br = gtk_label_new("Brighter");
    gtk_widget_set_valign(lbl_br, GTK_ALIGN_START);
    gtk_widget_add_css_class(lbl_br, "dim-label");
    GtkWidget *lbl_dk = gtk_label_new("Darker");
    gtk_widget_set_vexpand(lbl_dk, TRUE);
    gtk_widget_set_valign(lbl_dk, GTK_ALIGN_END);
    gtk_widget_add_css_class(lbl_dk, "dim-label");
    gtk_box_append(GTK_BOX(lbl_col), lbl_br);
    gtk_box_append(GTK_BOX(lbl_col), lbl_dk);
    gtk_box_append(GTK_BOX(row), lbl_col);

    s->scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0.5, 3.0, 0.00001);
    gtk_range_set_value(GTK_RANGE(s->scale), 2.2);
    gtk_range_set_inverted(GTK_RANGE(s->scale), TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(s->scale), FALSE);
    gtk_widget_set_size_request(s->scale, 40, 240);
    gtk_widget_set_vexpand(s->scale, TRUE);
    gtk_scale_add_mark(GTK_SCALE(s->scale), 2.2, GTK_POS_RIGHT, "Standard");
    gtk_box_append(GTK_BOX(row), s->scale);

    char val_buf[16]; snprintf(val_buf, sizeof(val_buf), "%.5f", 2.2);
    s->value_label = gtk_label_new(val_buf);
    gtk_widget_add_css_class(s->value_label, "title-3");
    gtk_widget_set_valign(s->value_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), s->value_label);

    s->spin = gtk_spin_button_new_with_range(0.5, 3.0, 0.00001);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->spin), 2.2);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s->spin), 5);
    gtk_widget_set_halign(s->spin, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), s->spin);

    s->status_label = gtk_label_new("◀ Original State — move slider to compare");
    gtk_label_set_wrap(GTK_LABEL(s->status_label), TRUE);
    gtk_widget_add_css_class(s->status_label, "dim-label");
    gtk_box_append(GTK_BOX(root), s->status_label);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), btn_row);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_sync_cancel), s);
    gtk_box_append(GTK_BOX(btn_row), cancel_btn);

    GtkWidget *confirm = gtk_button_new_with_label("Confirm Baseline");
    gtk_widget_add_css_class(confirm, "suggested-action");
    g_signal_connect(confirm, "clicked", G_CALLBACK(on_sync_confirm), s);
    gtk_box_append(GTK_BOX(btn_row), confirm);

    g_signal_connect(s->scale, "value-changed", G_CALLBACK(on_sync_scale_changed), s);
    g_signal_connect(s->spin,  "value-changed", G_CALLBACK(on_sync_spin_changed),  s);
    g_signal_connect(dlg, "close-request", G_CALLBACK(on_sync_close_request), s);

    g_object_set_data_full(G_OBJECT(dlg), "sync-widgets", s, g_free);
    gtk_window_present(GTK_WINDOW(dlg));
}

static void on_resync(GtkButton *btn, gpointer user_data) {
    (void)btn;
    show_sync_dialog((AppWidgets*)user_data);
}

/* ── UI construction ────────────────────────────────────────────────────── */
static void activate(GtkApplication *app, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;

    detect_outputs();
    if (g_n_outputs == 0) {
        g_n_outputs = 1;
        strncpy(g_outputs[0].name, "unknown", 255);
        g_outputs[0].icc[0] = '\0';
        g_outputs[0].priority = 1;
    }

    // Always start by selecting the first detected output
    w->cur_output = 0;
    const char *initial_out = g_outputs[0].name;

    const char *home = g_get_home_dir();
    char dir[512]; snprintf(dir, sizeof(dir), "%s" ICC_DIR, home);
    mkdir(dir, 0755);

    double saved = load_base_gamma(initial_out);
    w->base_gamma = (saved > 0) ? saved : 1.485;
    w->baseline_valid = TRUE;

    GtkWidget *win = gtk_application_window_new(app);
    w->main_window = win;
    gtk_window_set_title(GTK_WINDOW(win), "Gamma Control");
    gtk_window_set_default_size(GTK_WINDOW(win), 280, 640);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);
    gtk_widget_set_margin_top(root, 20);
    gtk_widget_set_margin_bottom(root, 20);
    gtk_window_set_child(GTK_WINDOW(win), root);

    GtkWidget *title = gtk_label_new("Gamma Control");
    gtk_widget_add_css_class(title, "title-2");
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), title);

    GtkStringList *slist = gtk_string_list_new(NULL);
    for (int i = 0; i < g_n_outputs; i++) gtk_string_list_append(slist, g_outputs[i].name);
    w->monitor_combo = gtk_drop_down_new(G_LIST_MODEL(slist), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->monitor_combo), 0);
    gtk_box_append(GTK_BOX(root), w->monitor_combo);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_vexpand(row, TRUE);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), row);

    GtkWidget *lbl_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(lbl_col, GTK_ALIGN_FILL);
    GtkWidget *lbl_dark = gtk_label_new("Brighter");
    gtk_widget_set_valign(lbl_dark, GTK_ALIGN_START);
    gtk_widget_add_css_class(lbl_dark, "dim-label");
    GtkWidget *lbl_bright = gtk_label_new("Darker");
    gtk_widget_set_vexpand(lbl_bright, TRUE);
    gtk_widget_set_valign(lbl_bright, GTK_ALIGN_END);
    gtk_widget_add_css_class(lbl_bright, "dim-label");
    gtk_box_append(GTK_BOX(lbl_col), lbl_dark);
    gtk_box_append(GTK_BOX(lbl_col), lbl_bright);
    gtk_box_append(GTK_BOX(row), lbl_col);

    double saved_mult = load_multiplier(initial_out);

    w->scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, GAMMA_MIN, GAMMA_MAX, GAMMA_STEP);
    gtk_range_set_value(GTK_RANGE(w->scale), saved_mult);
    gtk_range_set_inverted(GTK_RANGE(w->scale), TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(w->scale), FALSE);
    gtk_widget_set_size_request(w->scale, 40, 300);
    gtk_widget_set_vexpand(w->scale, TRUE);
    char mark_label[48]; snprintf(mark_label, sizeof(mark_label), "1.0 (baseline %.5f)", w->base_gamma);
    gtk_scale_add_mark(GTK_SCALE(w->scale), 1.0, GTK_POS_RIGHT, mark_label);
    gtk_box_append(GTK_BOX(row), w->scale);

    char val_buf[16]; snprintf(val_buf, sizeof(val_buf), "%.5f", saved_mult);
    w->value_label = gtk_label_new(val_buf);
    gtk_widget_add_css_class(w->value_label, "title-3");
    gtk_widget_set_valign(w->value_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), w->value_label);

    w->spin = gtk_spin_button_new_with_range(GAMMA_MIN, GAMMA_MAX, GAMMA_STEP);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->spin), saved_mult);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w->spin), 5);
    gtk_spin_button_set_increments(GTK_SPIN_BUTTON(w->spin), 0.00001, 0.001);
    gtk_widget_set_halign(w->spin, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), w->spin);

    w->status_label = gtk_label_new("Adjust multiplier (1.0 = baseline).");
    gtk_widget_set_halign(w->status_label, GTK_ALIGN_CENTER);
    gtk_label_set_wrap(GTK_LABEL(w->status_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(w->status_label), 32);
    gtk_widget_add_css_class(w->status_label, "dim-label");
    gtk_box_append(GTK_BOX(root), w->status_label);

    /* Row for Resync / Reset to Day 1 */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), btn_row);

    GtkWidget *rb = gtk_button_new_with_label("Reset");
    g_signal_connect(rb, "clicked", G_CALLBACK(on_reset), w);
    gtk_box_append(GTK_BOX(btn_row), rb);

    GtkWidget *sb = gtk_button_new_with_label("Resync Baseline");
    g_signal_connect(sb, "clicked", G_CALLBACK(on_resync), w);
    gtk_box_append(GTK_BOX(btn_row), sb);

    /* Row for Changing the default entirely */
    GtkWidget *change_default_btn = gtk_button_new_with_label("Change Default ICC Profile");
    gtk_widget_set_halign(change_default_btn, GTK_ALIGN_CENTER);
    g_signal_connect(change_default_btn, "clicked", G_CALLBACK(on_change_default_clicked), w);
    gtk_box_append(GTK_BOX(root), change_default_btn);

    /* Reset all and relaunch */
    GtkWidget *reset_all_btn = gtk_button_new_with_label("Reset All Settings & Relaunch");
    gtk_widget_add_css_class(reset_all_btn, "destructive-action");
    gtk_widget_set_halign(reset_all_btn, GTK_ALIGN_CENTER);
    g_signal_connect(reset_all_btn, "clicked", G_CALLBACK(on_reset_all), w);
    gtk_box_append(GTK_BOX(root), reset_all_btn);

    g_signal_connect(w->scale, "value-changed", G_CALLBACK(on_scale_changed), w);
    g_signal_connect(w->spin,  "value-changed", G_CALLBACK(on_spin_changed),  w);
    g_signal_connect(w->monitor_combo, "notify::selected", G_CALLBACK(on_monitor_changed), w);

    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char *argv[]) {
    /* Single-instance enforcement via lockfile */
    const char *home_env = getenv("HOME");
    if (home_env) {
        char lock_dir[512], lock_path[512];
        snprintf(lock_dir,  sizeof(lock_dir),  "%s/.config/gammactrl", home_env);
        snprintf(lock_path, sizeof(lock_path), "%s/.config/gammactrl/gammactrl.lock", home_env);
        mkdir(lock_dir, 0755);
        int lock_fd = open(lock_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (lock_fd >= 0) {
            if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
                fprintf(stderr, "[gammactrl] Already running.\n");
                close(lock_fd);
                return 1;
            }
            /* fd held open for process lifetime; O_CLOEXEC ensures it closes on execv */
        }
    }
    AppWidgets w = {0};
    GtkApplication *app = gtk_application_new("org.gammactrl", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &w);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
