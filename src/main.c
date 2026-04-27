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
#define GAMMA_MIN      0.001
#define GAMMA_MAX      3.0
#define GAMMA_STEP     0.00001
#define SLIDER_DEFAULT 1.0
#define NATIVE_GAMMA   1.48495

#define LOG(fmt, ...) fprintf(stderr, "[gammactrl] " fmt "\n", ##__VA_ARGS__)

/* ── Per-Monitor Persistence ────────────────────────────────────────────── */

static double load_double_config(const char *filename, double def, double lo, double hi) {
    FILE *f = fopen(filename, "r");
    if (!f) return def;
    double v = def;
    fscanf(f, "%lf", &v);
    fclose(f);
    return (v >= lo && v <= hi) ? v : def;
}

static void save_double_config(const char *filename, double val) {
    const char *home = g_get_home_dir();
    char dir[512];
    snprintf(dir, sizeof(dir), "%s" CONFIG_DIR, home);
    mkdir(dir, 0755);
    FILE *f = fopen(filename, "w");
    if (f) { fprintf(f, "%.5f\n", val); fclose(f); }
}

static double load_multiplier(const char *out_name) {
    const char *home = g_get_home_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/gammactrl/mult_%s", home, out_name);
    return load_double_config(path, SLIDER_DEFAULT, GAMMA_MIN, GAMMA_MAX);
}

static void save_multiplier(const char *out_name, double val) {
    const char *home = g_get_home_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/gammactrl/mult_%s", home, out_name);
    save_double_config(path, val);
}

static double load_sync_baseline(const char *out_name) {
    const char *home = g_get_home_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/gammactrl/sync_%s", home, out_name);
    return load_double_config(path, 1.0, 0.1, 5.0);
}

static void save_sync_baseline(const char *out_name, double val) {
    const char *home = g_get_home_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/gammactrl/sync_%s", home, out_name);
    save_double_config(path, val);
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
    int  icc_toggle;  /* alternates 0/1 per apply; -1 = uninitialized */
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
            if (strcmp(stored, "[NONE]") == 0)
                g_outputs[cur].icc[0] = '\0';
            else
                strncpy(g_outputs[cur].icc, stored, sizeof(g_outputs[cur].icc)-1);
            LOG("Output %d loaded persistent Day 1 profile: %s", cur, stored);
        }
        fclose(f);
    } else {
        if (strstr(g_outputs[cur].icc, "gammactrl_")) {
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
        /* Strip ANSI escape codes in-place */
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
            char name[256] = {0};
            if (sscanf(p, "%*d %255s", name) == 1 || sscanf(p, "%255s", name) == 1) {
                if (g_n_outputs < MAX_OUTPUTS) {
                    cur = g_n_outputs++;
                    strncpy(g_outputs[cur].name, name, sizeof(g_outputs[cur].name)-1);
                    g_outputs[cur].icc[0]    = '\0';
                    g_outputs[cur].icc_toggle = -1;
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
                if (strcmp(val, "none") != 0 && strlen(val) > 0)
                    strncpy(g_outputs[cur].icc, val, sizeof(g_outputs[cur].icc)-1);
            }
        }
    }
    pclose(f);

    for (int i = 0; i < g_n_outputs; i++)
        manage_day1_profile(i);

    {
        const char *home = g_get_home_dir();
        char icc_dir[512];
        snprintf(icc_dir, sizeof(icc_dir), "%s" ICC_DIR, home);
        mkdir(icc_dir, 0755);
        for (int i = 0; i < g_n_outputs; i++) {
            char p[512];
            snprintf(p, sizeof(p), "%s/gammactrl_%s_0.icc", icc_dir, g_outputs[i].name); remove(p);
            snprintf(p, sizeof(p), "%s/gammactrl_%s_1.icc", icc_dir, g_outputs[i].name); remove(p);
            LOG("Cleared stale ICC files for %s", g_outputs[i].name);
        }
    }

    LOG("Detected %d output(s).", g_n_outputs);
    return g_n_outputs;
}

static void apply_icc(const char *output_name, const char *icc_path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "kscreen-doctor output.%s.iccprofile.\"%s\"",
             output_name, icc_path ? icc_path : "");
    LOG("apply_icc cmd: %s", cmd);
    int ret = system(cmd);
    LOG("apply_icc exit: %d", ret);
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

static int copy_and_patch_icc(const char *src_path, const char *dst_path, double slider_val) {
    FILE *src = fopen(src_path, "rb");
    if (!src) return -1;
    fseek(src, 0, SEEK_END);
    long file_size = ftell(src);
    fseek(src, 0, SEEK_SET);

    size_t padded_sz = file_size + 2048;
    uint8_t *data = calloc(1, padded_sz);
    if (!data) { fclose(src); return -1; }

    if ((long)fread(data, 1, file_size, src) != file_size || file_size < 132) {
        free(data); fclose(src); return -1;
    }
    fclose(src);

    uint32_t profile_size = (data[0]<<24)|(data[1]<<16)|(data[2]<<8)|data[3];
    if (profile_size != (uint32_t)file_size) { free(data); return -1; }
    uint32_t tag_count = (data[128]<<24)|(data[129]<<16)|(data[130]<<8)|data[131];
    if (tag_count == 0 || tag_count > 100) { free(data); return -1; }

    uint8_t *tag_table  = data + 132;
    int      vcgt_found = 0;
    uint32_t vcgt_offset = 0, vcgt_size = 0;

    for (uint32_t i = 0; i < tag_count; i++) {
        uint8_t *entry = tag_table + i * 12;
        if (memcmp(entry, "vcgt", 4) == 0) {
            vcgt_found  = 1;
            vcgt_offset = (entry[4]<<24)|(entry[5]<<16)|(entry[6]<<8)|entry[7];
            vcgt_size   = (entry[8]<<24)|(entry[9]<<16)|(entry[10]<<8)|entry[11];
            break;
        }
    }

    int patched = 0;

    if (vcgt_found && vcgt_offset + vcgt_size <= (uint32_t)file_size && vcgt_size >= 18) {
        uint32_t vcgt_type = (data[vcgt_offset+8]<<24)|(data[vcgt_offset+9]<<16)|(data[vcgt_offset+10]<<8)|data[vcgt_offset+11];
        if (vcgt_type == 0) {
            uint16_t nchannels = (data[vcgt_offset+12]<<8)|data[vcgt_offset+13];
            uint16_t nentries  = (data[vcgt_offset+14]<<8)|data[vcgt_offset+15];
            uint16_t entry_sz  = (data[vcgt_offset+16]<<8)|data[vcgt_offset+17];

            if (entry_sz == 2 && vcgt_offset + 18 + nchannels * nentries * 2 <= (uint32_t)file_size) {
                uint8_t *tbl = data + vcgt_offset + 18;
                for (int i = 0; i < nchannels * nentries; i++) {
                    double v = pow(((tbl[i*2]<<8) | tbl[i*2+1]) / 65535.0, 1.0 / slider_val);
                    if (v < 0) v = 0;
                    if (v > 1) v = 1;
                    uint16_t nv = (uint16_t)(v * 65535.0);
                    tbl[i*2] = nv >> 8; tbl[i*2+1] = nv & 0xff;
                }
                patched = 1;
            }
        }
    }

    if (!patched) {
        uint8_t *new_data = calloc(1, padded_sz);
        if (!new_data) { free(data); return -1; }

        memcpy(new_data, data, 132);
        uint32_t new_tag_count = tag_count + 1;
        write_be32(new_data + 128, new_tag_count);
        memcpy(new_data + 132, data + 132, tag_count * 12);

        uint32_t old_data_offset = 132 + tag_count * 12;
        uint32_t new_data_offset = 132 + new_tag_count * 12;
        memcpy(new_data + new_data_offset, data + old_data_offset, file_size - old_data_offset);

        for (uint32_t i = 0; i < tag_count; i++) {
            uint8_t *entry = new_data + 132 + i * 12;
            uint32_t old_off = (entry[4]<<24)|(entry[5]<<16)|(entry[6]<<8)|entry[7];
            write_be32(entry + 4, old_off + 12);
        }

        size_t vcgt_len;
        uint8_t *vcgt = build_vcgt_tag(slider_val, &vcgt_len);
        if (!vcgt) { free(new_data); free(data); return -1; }

        uint32_t appended_offset = ((file_size + 12) + 3) & ~3;
        memcpy(new_data + appended_offset, vcgt, vcgt_len);
        free(vcgt);

        uint8_t *new_entry = new_data + 132 + tag_count * 12;
        memcpy(new_entry, "vcgt", 4);
        write_be32(new_entry + 4, appended_offset);
        write_be32(new_entry + 8, (uint32_t)vcgt_len);

        uint32_t final_size = appended_offset + vcgt_len;
        write_be32(new_data, final_size);

        free(data);
        data      = new_data;
        file_size = final_size;
    }

    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { free(data); return -1; }
    fwrite(data, 1, file_size, dst);
    fclose(dst);
    free(data);
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

    write_be32(b, (uint32_t)tot);
    memset(b+4, ' ', 4);
    write_be32(b+8, 0x02100000);
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
    GtkWidget    *monitor_combo;
    GtkWidget    *scale;
    GtkWidget    *spin;
    GtkWidget    *value_label;
    GtkWidget    *status_label;
    GtkWidget    *main_window;
    int           cur_output;
    double        sync_baseline;
    double        profile_target;
    gboolean      baseline_valid;
    GFileMonitor *drm_monitor;
    guint         hotplug_debounce_id;
} AppWidgets;

static void show_sync_dialog(AppWidgets *w);
static void do_apply(AppWidgets *w);
static void on_spin_changed(GtkSpinButton *spin, gpointer user_data);

/* ── Main apply function ────────────────────────────────────────────────── */
static int apply_gamma(AppWidgets *w, double total_mult) {
    const char *out      = g_outputs[w->cur_output].name;
    const char *orig_icc = g_outputs[w->cur_output].icc;
    const char *home     = g_get_home_dir();

    g_outputs[w->cur_output].icc_toggle = (g_outputs[w->cur_output].icc_toggle + 1) % 2;
    int toggle = g_outputs[w->cur_output].icc_toggle;

    char active[512], stale[512];
    snprintf(active, sizeof(active), "%s/.local/share/icc/gammactrl_%s_%d.icc", home, out, toggle);
    snprintf(stale,  sizeof(stale),  "%s/.local/share/icc/gammactrl_%s_%d.icc", home, out, toggle ^ 1);

    LOG("apply_gamma: toggle=%d active=%s", toggle, active);

    if (orig_icc[0] != '\0' && access(orig_icc, R_OK) == 0) {
        if (copy_and_patch_icc(orig_icc, active, total_mult) == 0) {
            remove(stale);
            apply_icc(out, active);
            return 0;
        }
    }

    double hardware_gamma = w->profile_target * NATIVE_GAMMA * total_mult;
    int r = write_icc_from_scratch(active, hardware_gamma);
    LOG("write_icc_from_scratch returned %d, file exists: %d", r, access(active, F_OK) == 0);
    if (r != 0) return -1;
    remove(stale);
    apply_icc(out, active);
    return 0;
}

/* ── Callbacks ─────────────────────────────────────────────────────────── */
static void do_apply(AppWidgets *w) {
    if (!w->baseline_valid) return;
    double slider_val = gtk_range_get_value(GTK_RANGE(w->scale));
    save_multiplier(g_outputs[w->cur_output].name, slider_val);

    double total_mult = w->sync_baseline * slider_val;
    LOG("do_apply_val: slider=%.5f sync=%.5f target=%.3f total_mult=%.5f toggle=%d",
        slider_val, w->sync_baseline, w->profile_target, total_mult,
        g_outputs[w->cur_output].icc_toggle);

    if (apply_gamma(w, total_mult) != 0) {
        gtk_label_set_text(GTK_LABEL(w->status_label), "Error applying gamma");
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Multiplier: %.5f  (Baseline %.5f - Original Gamma %.3f)",
             slider_val, w->sync_baseline, w->profile_target);
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
    debounce_id = g_timeout_add(250, debounce_cb, w);
}

static void sync_value_label(AppWidgets *w, double val) {
    char buf[16]; snprintf(buf, sizeof(buf), "%.5f", val);
    gtk_label_set_text(GTK_LABEL(w->value_label), buf);
}
static gboolean on_scale_changed(GtkRange *scale, GtkScrollType scroll, double val, gpointer user_data) {
    (void)scale; (void)scroll;
    AppWidgets *w = (AppWidgets*)user_data;
    g_signal_handlers_block_by_func(w->spin, on_spin_changed, w);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->spin), val);
    g_signal_handlers_unblock_by_func(w->spin, on_spin_changed, w);
    sync_value_label(w, val);
    schedule_apply(w);
    return FALSE;
}
static void on_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    double val = gtk_spin_button_get_value(spin);
    g_signal_handlers_block_by_func(w->scale, on_scale_changed, w);
    gtk_range_set_value(GTK_RANGE(w->scale), val);
    g_signal_handlers_unblock_by_func(w->scale, on_scale_changed, w);
    sync_value_label(w, val);
    schedule_apply(w);
}

/* ── Read gamma from ICC file ────────────────────────────────────────────── */

/* Returns the display target gamma encoded in an ICC file, normalized to
 * NATIVE_GAMMA and rounded to 3 decimal places. Returns 1.0 if not found. */
static double read_icc_gamma(const char *icc_path) {
    if (!icc_path || !icc_path[0]) return 1.0;

    FILE *f = fopen(icc_path, "rb");
    if (!f) return 1.0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 132) { fclose(f); return 1.0; }

    uint8_t *data = malloc(sz);
    if (!data) { fclose(f); return 1.0; }
    if ((long)fread(data, 1, sz, f) != sz) { free(data); fclose(f); return 1.0; }
    fclose(f);

    uint32_t tag_count = (data[128]<<24)|(data[129]<<16)|(data[130]<<8)|data[131];
    if (tag_count > 100) { free(data); return 1.0; }

    uint8_t *tt = data + 132;
    double gamma = NATIVE_GAMMA;
    int found = 0;

    /* FIRST PASS: vcgt tag (Hardware Calibration) */
    for (uint32_t i = 0; i < tag_count && !found; i++) {
        uint8_t *entry = tt + i * 12;
        if (memcmp(entry, "vcgt", 4) != 0) continue;
        uint32_t offset = (entry[4]<<24)|(entry[5]<<16)|(entry[6]<<8)|entry[7];
        uint32_t tsize  = (entry[8]<<24)|(entry[9]<<16)|(entry[10]<<8)|entry[11];
        if (tsize < 18 || (long)(offset + tsize) > sz || memcmp(data + offset, "vcgt", 4) != 0) continue;
        uint32_t vcgt_type = (data[offset+8]<<24)|(data[offset+9]<<16)|(data[offset+10]<<8)|data[offset+11];
        if (vcgt_type != 0) continue;
        uint16_t nchannels = (data[offset+12]<<8)|data[offset+13];
        uint16_t nentries  = (data[offset+14]<<8)|data[offset+15];
        uint16_t entry_sz  = (data[offset+16]<<8)|data[offset+17];
        if (nchannels < 1 || nentries < 2 || entry_sz != 2) continue;
        if ((long)(offset + 18 + nchannels * nentries * 2) > sz) continue;
        int mid = nentries / 2;
        uint8_t *tbl = data + offset + 18;
        double mid_val = ((tbl[mid*2]<<8)|tbl[mid*2+1]) / 65535.0;
        double mid_in  = (double)mid / (nentries - 1);
        if (mid_val > 0.0 && mid_in > 0.0 && mid_val < 1.0 && mid_in < 1.0) {
            double g = log(mid_in) / log(mid_val);
            if (g > 0.0001 && g < 100.0) { gamma = g; found = 1; }
        }
    }

    /* SECOND PASS: rTRC tag (Software Color Space), only if vcgt not found */
    for (uint32_t i = 0; i < tag_count && !found; i++) {
        uint8_t *entry = tt + i * 12;
        if (memcmp(entry, "rTRC", 4) != 0) continue;
        uint32_t offset = (entry[4]<<24)|(entry[5]<<16)|(entry[6]<<8)|entry[7];
        uint32_t tsize  = (entry[8]<<24)|(entry[9]<<16)|(entry[10]<<8)|entry[11];
        if (tsize < 12 || (long)(offset + tsize) > sz || memcmp(data + offset, "para", 4) != 0) continue;
        uint16_t ftype = (data[offset+8]<<8)|data[offset+9];
        if (ftype != 0) continue;
        int32_t raw = (int32_t)((data[offset+12]<<24)|(data[offset+13]<<16)|
        (data[offset+14]<<8)  | data[offset+15]);
        double g = raw / 65536.0;
        if (g > 0.0001 && g < 100.0) { gamma = g; found = 1; }
    }

    free(data);
    LOG("read_icc_gamma(%s) raw = %.5f (found=%d)", icc_path, gamma, found);
    return round((gamma / NATIVE_GAMMA) * 1000.0) / 1000.0;
}

/* ── Monitor switching ──────────────────────────────────────────────────── */
static void load_output_into_ui(AppWidgets *w, int idx) {
    const char *out_name = g_outputs[idx].name;
    w->sync_baseline  = load_sync_baseline(out_name);
    w->profile_target = read_icc_gamma(g_outputs[idx].icc);
    w->baseline_valid = TRUE;

    double saved_mult = load_multiplier(out_name);
    g_signal_handlers_block_by_func(w->scale, on_scale_changed, w);
    g_signal_handlers_block_by_func(w->spin,  on_spin_changed,  w);
    gtk_range_set_value(GTK_RANGE(w->scale), saved_mult);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->spin), saved_mult);
    g_signal_handlers_unblock_by_func(w->scale, on_scale_changed, w);
    g_signal_handlers_unblock_by_func(w->spin,  on_spin_changed,  w);
    sync_value_label(w, saved_mult);
}

static void on_monitor_changed(GtkDropDown *dd, GParamSpec *ps, gpointer user_data) {
    (void)ps;
    AppWidgets *w = (AppWidgets*)user_data;
    w->cur_output = (int)gtk_drop_down_get_selected(dd);
    load_output_into_ui(w, w->cur_output);
    do_apply(w);
}

static void on_reset(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *w = (AppWidgets*)user_data;
    g_signal_handlers_block_by_func(w->scale, on_scale_changed, w);
    g_signal_handlers_block_by_func(w->spin,  on_spin_changed,  w);
    gtk_range_set_value(GTK_RANGE(w->scale), SLIDER_DEFAULT);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->spin), SLIDER_DEFAULT);
    g_signal_handlers_unblock_by_func(w->scale, on_scale_changed, w);
    g_signal_handlers_unblock_by_func(w->spin,  on_spin_changed,  w);
    sync_value_label(w, SLIDER_DEFAULT);
    save_multiplier(g_outputs[w->cur_output].name, SLIDER_DEFAULT);
    do_apply(w);
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

    if (new_icc && new_icc[0])
        strncpy(g_outputs[cur].icc, new_icc, sizeof(g_outputs[cur].icc)-1);
    else
        g_outputs[cur].icc[0] = '\0';

    apply_icc(g_outputs[cur].name, g_outputs[cur].icc);

    w->sync_baseline  = 1.0;
    w->baseline_valid = TRUE;
    save_sync_baseline(g_outputs[cur].name, 1.0);

    g_signal_handlers_block_by_func(w->scale, on_scale_changed, w);
    g_signal_handlers_block_by_func(w->spin,  on_spin_changed,  w);
    gtk_range_set_value(GTK_RANGE(w->scale), SLIDER_DEFAULT);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->spin), SLIDER_DEFAULT);
    g_signal_handlers_unblock_by_func(w->scale, on_scale_changed, w);
    g_signal_handlers_unblock_by_func(w->spin,  on_spin_changed,  w);
    sync_value_label(w, SLIDER_DEFAULT);
    save_multiplier(g_outputs[cur].name, SLIDER_DEFAULT);

    w->profile_target = read_icc_gamma(g_outputs[cur].icc);
    do_apply(w);

    if (parent_dlg) gtk_window_destroy(GTK_WINDOW(parent_dlg));
}

static void on_native_day1_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    GtkWidget *dlg = g_object_get_data(G_OBJECT(btn), "dialog");
    apply_default_change(w, dlg, "");
}

static void on_file_dialog_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
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
    gtk_file_dialog_open(dialog, GTK_WINDOW(w->main_window), NULL, on_file_dialog_response, w);
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
    gtk_widget_set_margin_start(box, 20); gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 20);   gtk_widget_set_margin_bottom(box, 20);
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

    GtkWidget *btn_native = gtk_button_new_with_label("Set to no profile");
    g_object_set_data(G_OBJECT(btn_native), "dialog", dlg);
    g_signal_connect(btn_native, "clicked", G_CALLBACK(on_native_day1_clicked), w);
    gtk_box_append(GTK_BOX(box), btn_native);

    gtk_window_present(GTK_WINDOW(dlg));
}

/* ── Reset All and Relaunch ────────────────────────────────────────────── */
static void on_reset_all_confirm(GtkButton *btn, gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    GtkWidget *dlg = g_object_get_data(G_OBJECT(btn), "dialog");
    if (dlg) gtk_window_destroy(GTK_WINDOW(dlg));

    const char *home = g_get_home_dir();
    char path[1024];

    for (int i = 0; i < g_n_outputs; i++) {
        const char *out      = g_outputs[i].name;
        const char *orig_icc = g_outputs[i].icc;
        apply_icc(out, (orig_icc[0] && access(orig_icc, R_OK) == 0) ? orig_icc : "");

        snprintf(path, sizeof(path), "%s/.local/share/icc/gammactrl_%s_0.icc",      home, out); remove(path);
        snprintf(path, sizeof(path), "%s/.local/share/icc/gammactrl_%s_1.icc",      home, out); remove(path);
        snprintf(path, sizeof(path), "%s/.local/share/icc/gammactrl_sync_%s_0.icc", home, out); remove(path);
        snprintf(path, sizeof(path), "%s/.local/share/icc/gammactrl_sync_%s_1.icc", home, out); remove(path);
        snprintf(path, sizeof(path), "%s/.config/gammactrl/sync_%s",                home, out); remove(path);
        snprintf(path, sizeof(path), "%s/.config/gammactrl/mult_%s",                home, out); remove(path);
        snprintf(path, sizeof(path), "%s/.config/gammactrl/day1_%s",                home, out); remove(path);
    }

    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        snprintf(path, sizeof(path), "%s/.config/gammactrl/gammactrl.lock", home);
        remove(path);
        char *args[] = { exe_path, NULL };
        execv(exe_path, args);
    }
    (void)w;
}

static void on_reset_all(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *w = (AppWidgets*)user_data;

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Reset All Settings?");
    gtk_window_set_default_size(GTK_WINDOW(dlg), 320, -1);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(w->main_window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);   gtk_widget_set_margin_bottom(box, 16);
    gtk_window_set_child(GTK_WINDOW(dlg), box);

    GtkWidget *lbl = gtk_label_new("This will erase all saved settings for every monitor and relaunch. This cannot be undone.");
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 36);
    gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), lbl);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), btn_row);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel_btn, "clicked", G_CALLBACK(gtk_window_destroy), dlg);
    gtk_box_append(GTK_BOX(btn_row), cancel_btn);

    GtkWidget *confirm_btn = gtk_button_new_with_label("Reset & Relaunch");
    gtk_widget_add_css_class(confirm_btn, "destructive-action");
    g_object_set_data(G_OBJECT(confirm_btn), "dialog", dlg);
    g_signal_connect(confirm_btn, "clicked", G_CALLBACK(on_reset_all_confirm), w);
    gtk_box_append(GTK_BOX(btn_row), confirm_btn);

    gtk_window_present(GTK_WINDOW(dlg));
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
    int         preview_toggle;
    char        preview_icc[2][512];
} SyncWidgets;

static void sync_apply_new(SyncWidgets *s) {
    double sync_val = gtk_range_get_value(GTK_RANGE(s->scale));
    const char *out = g_outputs[s->w->cur_output].name;
    s->preview_toggle = (s->preview_toggle + 1) % 2;

    if (s->orig_icc[0] != '\0' && access(s->orig_icc, R_OK) == 0)
        copy_and_patch_icc(s->orig_icc, s->preview_icc[s->preview_toggle], sync_val);
    else
        write_icc_from_scratch(s->preview_icc[s->preview_toggle],
                               s->w->profile_target * NATIVE_GAMMA * sync_val);

        apply_icc(out, s->preview_icc[s->preview_toggle]);
}

static void sync_restore_orig(SyncWidgets *s) {
    apply_icc(g_outputs[s->w->cur_output].name, s->orig_icc);
}

static void sync_set_status(SyncWidgets *s, double sync_val) {
    char msg[64];
    snprintf(msg, sizeof(msg), "▶ Sync Multiplier: %.5f", sync_val);
    gtk_label_set_text(GTK_LABEL(s->status_label), msg);
}

static gboolean sync_toggle_cb(gpointer user_data) {
    SyncWidgets *s = (SyncWidgets*)user_data;
    if (!GTK_IS_WIDGET(s->dialog)) return G_SOURCE_REMOVE;
    s->toggle_phase ^= 1;
    if (s->toggle_phase == 0) {
        sync_restore_orig(s);
        gtk_label_set_text(GTK_LABEL(s->status_label), "◀ Original State");
    } else {
        sync_apply_new(s);
        sync_set_status(s, gtk_range_get_value(GTK_RANGE(s->scale)));
    }
    return G_SOURCE_CONTINUE;
}

static guint sync_debounce_id = 0;
static gboolean sync_debounce_cb(gpointer user_data) {
    sync_debounce_id = 0;
    SyncWidgets *s = (SyncWidgets*)user_data;
    if (!GTK_IS_WIDGET(s->dialog)) return G_SOURCE_REMOVE;
    sync_apply_new(s);
    sync_set_status(s, gtk_range_get_value(GTK_RANGE(s->scale)));
    s->toggle_phase = 1;
    if (s->toggle_timer_id) g_source_remove(s->toggle_timer_id);
    s->toggle_timer_id = g_timeout_add(1000, sync_toggle_cb, s);
    return G_SOURCE_REMOVE;
}

static void sync_slider_updated(SyncWidgets *s, double val) {
    char buf[16]; snprintf(buf, sizeof(buf), "%.5f", val);
    gtk_label_set_text(GTK_LABEL(s->value_label), buf);
    if (s->toggle_timer_id) { g_source_remove(s->toggle_timer_id); s->toggle_timer_id = 0; }
    if (sync_debounce_id) g_source_remove(sync_debounce_id);
    sync_debounce_id = g_timeout_add(400, sync_debounce_cb, s);
}

static void on_sync_scale_changed(GtkRange *scale, gpointer user_data) {
    SyncWidgets *s = (SyncWidgets*)user_data;
    if (s->updating) return;
    double val = gtk_range_get_value(scale);
    s->updating = TRUE;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->spin), val);
    s->updating = FALSE;
    sync_slider_updated(s, val);
}
static void on_sync_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    SyncWidgets *s = (SyncWidgets*)user_data;
    if (s->updating) return;
    double val = gtk_spin_button_get_value(spin);
    s->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(s->scale), val);
    s->updating = FALSE;
    sync_slider_updated(s, val);
}

static void sync_cleanup(SyncWidgets *s) {
    if (s->toggle_timer_id) { g_source_remove(s->toggle_timer_id); s->toggle_timer_id = 0; }
    if (sync_debounce_id)   { g_source_remove(sync_debounce_id);   sync_debounce_id = 0; }
    if (s->preview_icc[0][0]) remove(s->preview_icc[0]);
    if (s->preview_icc[1][0]) remove(s->preview_icc[1]);
}

static void on_sync_confirm(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SyncWidgets *s = (SyncWidgets*)user_data;
    double chosen_sync = gtk_range_get_value(GTK_RANGE(s->scale));
    sync_cleanup(s);

    const char *out_name = g_outputs[s->w->cur_output].name;
    save_sync_baseline(out_name, chosen_sync);
    s->w->sync_baseline  = chosen_sync;
    s->w->baseline_valid = TRUE;

    g_signal_handlers_block_by_func(s->w->scale, on_scale_changed, s->w);
    g_signal_handlers_block_by_func(s->w->spin,  on_spin_changed,  s->w);
    gtk_range_set_value(GTK_RANGE(s->w->scale), SLIDER_DEFAULT);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->w->spin), SLIDER_DEFAULT);
    g_signal_handlers_unblock_by_func(s->w->scale, on_scale_changed, s->w);
    g_signal_handlers_unblock_by_func(s->w->spin,  on_spin_changed,  s->w);
    sync_value_label(s->w, SLIDER_DEFAULT);
    save_multiplier(out_name, SLIDER_DEFAULT);

    do_apply(s->w);
    gtk_window_destroy(GTK_WINDOW(s->dialog));
}

static void sync_close(SyncWidgets *s) {
    sync_cleanup(s);
    if (s->w->baseline_valid)
        do_apply(s->w);
    else
        sync_restore_orig(s);
    gtk_window_destroy(GTK_WINDOW(s->dialog));
}

static void on_sync_cancel(GtkButton *btn, gpointer user_data) {
    (void)btn;
    sync_close((SyncWidgets*)user_data);
}

static gboolean on_sync_close_request(GtkWindow *win, gpointer user_data) {
    (void)win;
    sync_close((SyncWidgets*)user_data);
    return FALSE;
}

static void show_sync_dialog(AppWidgets *w) {
    SyncWidgets *s = g_new0(SyncWidgets, 1);
    s->w = w;

    const char *out_name = g_outputs[w->cur_output].name;
    strncpy(s->orig_icc, g_outputs[w->cur_output].icc, sizeof(s->orig_icc)-1);

    const char *home = g_get_home_dir();
    snprintf(s->preview_icc[0], sizeof(s->preview_icc[0]), "%s/.local/share/icc/gammactrl_sync_%s_0.icc", home, out_name);
    snprintf(s->preview_icc[1], sizeof(s->preview_icc[1]), "%s/.local/share/icc/gammactrl_sync_%s_1.icc", home, out_name);
    s->preview_toggle = -1;
    s->toggle_phase   = 0;
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
    gtk_widget_set_margin_start(root, 20); gtk_widget_set_margin_end(root, 20);
    gtk_widget_set_margin_top(root, 20);   gtk_widget_set_margin_bottom(root, 20);
    gtk_window_set_child(GTK_WINDOW(dlg), root);

    GtkWidget *heading = gtk_label_new("Sync Baseline");
    gtk_widget_add_css_class(heading, "title-2");
    gtk_widget_set_halign(heading, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), heading);

    char inst_text[700];
    snprintf(inst_text, sizeof(inst_text),
             "Sync the default gamma baseline of the program to match your screen in case it is misaligned.\n\n"
             "Your screen will alternate every second between your default profile and the sync multiplier.\n\n"
             "Original State: %s\n\n"
             "Drag the slider from 1.0 to correct the baseline.",
             s->orig_icc[0] ? s->orig_icc : "no profile");
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
    GtkWidget *lbl_br  = gtk_label_new("Brighter");
    gtk_widget_set_valign(lbl_br, GTK_ALIGN_START);
    gtk_widget_add_css_class(lbl_br, "dim-label");
    GtkWidget *lbl_dk = gtk_label_new("Darker");
    gtk_widget_set_vexpand(lbl_dk, TRUE);
    gtk_widget_set_valign(lbl_dk, GTK_ALIGN_END);
    gtk_widget_add_css_class(lbl_dk, "dim-label");
    gtk_box_append(GTK_BOX(lbl_col), lbl_br);
    gtk_box_append(GTK_BOX(lbl_col), lbl_dk);
    gtk_box_append(GTK_BOX(row), lbl_col);

    s->scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, GAMMA_MIN, GAMMA_MAX, GAMMA_STEP);
    gtk_range_set_value(GTK_RANGE(s->scale), 1.0);
    gtk_range_set_inverted(GTK_RANGE(s->scale), TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(s->scale), FALSE);
    gtk_widget_set_size_request(s->scale, 40, 240);
    gtk_widget_set_vexpand(s->scale, TRUE);
    gtk_scale_add_mark(GTK_SCALE(s->scale), 1.0, GTK_POS_RIGHT, "1.0 (Neutral)");
    gtk_box_append(GTK_BOX(row), s->scale);

    s->value_label = gtk_label_new("1.00000");
    gtk_widget_add_css_class(s->value_label, "title-3");
    gtk_widget_set_valign(s->value_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), s->value_label);

    s->spin = gtk_spin_button_new_with_range(GAMMA_MIN, GAMMA_MAX, GAMMA_STEP);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->spin), 1.0);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s->spin), 5);
    gtk_widget_set_halign(s->spin, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), s->spin);

    s->status_label = gtk_label_new("◀ Original State");
    gtk_label_set_wrap(GTK_LABEL(s->status_label), TRUE);
    gtk_widget_add_css_class(s->status_label, "dim-label");
    gtk_box_append(GTK_BOX(root), s->status_label);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), btn_row);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_sync_cancel), s);
    gtk_box_append(GTK_BOX(btn_row), cancel_btn);

    GtkWidget *confirm = gtk_button_new_with_label("Confirm Sync");
    gtk_widget_add_css_class(confirm, "suggested-action");
    g_signal_connect(confirm, "clicked", G_CALLBACK(on_sync_confirm), s);
    gtk_box_append(GTK_BOX(btn_row), confirm);

    g_signal_connect(s->scale, "value-changed",  G_CALLBACK(on_sync_scale_changed), s);
    g_signal_connect(s->spin,  "value-changed",  G_CALLBACK(on_sync_spin_changed),  s);
    g_signal_connect(dlg,      "close-request",  G_CALLBACK(on_sync_close_request), s);

    g_object_set_data_full(G_OBJECT(dlg), "sync-widgets", s, g_free);
    gtk_window_present(GTK_WINDOW(dlg));
}

static void on_resync(GtkButton *btn, gpointer user_data) {
    (void)btn;
    show_sync_dialog((AppWidgets*)user_data);
}

/* ── Hotplug detection ──────────────────────────────────────────────────── */

static void rebuild_monitor_combo(AppWidgets *w) {
    char saved_name[256];
    strncpy(saved_name, g_outputs[w->cur_output].name, sizeof(saved_name) - 1);

    detect_outputs();
    if (g_n_outputs == 0) {
        g_n_outputs = 1;
        strncpy(g_outputs[0].name, "unknown", 255);
        g_outputs[0].icc[0]    = '\0';
        g_outputs[0].icc_toggle = -1;
    }

    GtkStringList *slist = gtk_string_list_new(NULL);
    for (int i = 0; i < g_n_outputs; i++)
        gtk_string_list_append(slist, g_outputs[i].name);

    g_signal_handlers_block_by_func(w->monitor_combo, on_monitor_changed, w);
    gtk_drop_down_set_model(GTK_DROP_DOWN(w->monitor_combo), G_LIST_MODEL(slist));
    g_object_unref(slist);

    int new_sel = 0;
    for (int i = 0; i < g_n_outputs; i++) {
        if (strcmp(g_outputs[i].name, saved_name) == 0) { new_sel = i; break; }
    }
    w->cur_output = new_sel;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->monitor_combo), (guint)new_sel);
    g_signal_handlers_unblock_by_func(w->monitor_combo, on_monitor_changed, w);

    char msg[64];
    snprintf(msg, sizeof(msg), "Display change detected — %d output(s) found.", g_n_outputs);
    gtk_label_set_text(GTK_LABEL(w->status_label), msg);
}

static gboolean hotplug_debounce_cb(gpointer user_data) {
    AppWidgets *w = (AppWidgets*)user_data;
    w->hotplug_debounce_id = 0;
    rebuild_monitor_combo(w);
    return G_SOURCE_REMOVE;
}

static void on_drm_changed(GFileMonitor *monitor, GFile *file, GFile *other_file,
                           GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file; (void)event_type;
    AppWidgets *w = (AppWidgets*)user_data;
    if (w->hotplug_debounce_id) g_source_remove(w->hotplug_debounce_id);
    w->hotplug_debounce_id = g_timeout_add(1000, hotplug_debounce_cb, w);
                           }

                           static void setup_hotplug_monitor(AppWidgets *w) {
                               GFile  *drm_dir = g_file_new_for_path("/sys/class/drm");
                               GError *err     = NULL;
                               w->drm_monitor  = g_file_monitor_directory(drm_dir, G_FILE_MONITOR_WATCH_MOVES, NULL, &err);
                               g_object_unref(drm_dir);
                               if (err) {
                                   g_error_free(err); err = NULL;
                                   drm_dir        = g_file_new_for_path("/sys/class/drm");
                                   w->drm_monitor = g_file_monitor_directory(drm_dir, G_FILE_MONITOR_NONE, NULL, &err);
                                   g_object_unref(drm_dir);
                                   if (err) {
                                       LOG("Hotplug monitor unavailable: %s", err->message);
                                       g_error_free(err);
                                       return;
                                   }
                               }
                               g_file_monitor_set_rate_limit(w->drm_monitor, 500);
                               g_signal_connect(w->drm_monitor, "changed", G_CALLBACK(on_drm_changed), w);
                               LOG("Hotplug monitor active on /sys/class/drm");
                           }

                           /* ── UI construction ────────────────────────────────────────────────────── */
                           static void activate(GtkApplication *app, gpointer user_data) {
                               AppWidgets *w = (AppWidgets*)user_data;

                               detect_outputs();
                               if (g_n_outputs == 0) {
                                   g_n_outputs = 1;
                                   strncpy(g_outputs[0].name, "unknown", 255);
                                   g_outputs[0].icc[0]    = '\0';
                                   g_outputs[0].icc_toggle = -1;
                               }

                               w->cur_output = 0;
                               const char *home = g_get_home_dir();
                               char dir[512]; snprintf(dir, sizeof(dir), "%s" ICC_DIR, home);
                               mkdir(dir, 0755);

                               GtkWidget *win = gtk_application_window_new(app);
                               w->main_window = win;
                               gtk_window_set_title(GTK_WINDOW(win), "Gamma Control");
                               gtk_window_set_default_size(GTK_WINDOW(win), 280, 640);
                               gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

                               GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
                               gtk_widget_set_margin_start(root, 20); gtk_widget_set_margin_end(root, 20);
                               gtk_widget_set_margin_top(root, 20);   gtk_widget_set_margin_bottom(root, 20);
                               gtk_window_set_child(GTK_WINDOW(win), root);

                               GtkWidget *title = gtk_label_new("Gamma Control");
                               gtk_widget_add_css_class(title, "title-2");
                               gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
                               gtk_box_append(GTK_BOX(root), title);

                               GtkStringList *slist = gtk_string_list_new(NULL);
                               for (int i = 0; i < g_n_outputs; i++) gtk_string_list_append(slist, g_outputs[i].name);
                               w->monitor_combo = gtk_drop_down_new(G_LIST_MODEL(slist), NULL);
                               gtk_drop_down_set_selected(GTK_DROP_DOWN(w->monitor_combo), 0);

                               GtkGesture *click_gesture = gtk_gesture_click_new();
                               gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), GDK_BUTTON_PRIMARY);
                               g_signal_connect_swapped(click_gesture, "pressed", G_CALLBACK(rebuild_monitor_combo), w);
                               gtk_widget_add_controller(w->monitor_combo, GTK_EVENT_CONTROLLER(click_gesture));

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

                               w->scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, GAMMA_MIN, GAMMA_MAX, GAMMA_STEP);
                               gtk_range_set_inverted(GTK_RANGE(w->scale), TRUE);
                               gtk_scale_set_draw_value(GTK_SCALE(w->scale), FALSE);
                               gtk_widget_set_size_request(w->scale, 40, 300);
                               gtk_widget_set_vexpand(w->scale, TRUE);
                               gtk_scale_add_mark(GTK_SCALE(w->scale), 1.0, GTK_POS_RIGHT, "1.0 (Neutral)");
                               gtk_box_append(GTK_BOX(row), w->scale);

                               w->value_label = gtk_label_new("");
                               gtk_widget_add_css_class(w->value_label, "title-3");
                               gtk_widget_set_valign(w->value_label, GTK_ALIGN_CENTER);
                               gtk_box_append(GTK_BOX(row), w->value_label);

                               w->spin = gtk_spin_button_new_with_range(GAMMA_MIN, GAMMA_MAX, GAMMA_STEP);
                               gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w->spin), 5);
                               gtk_spin_button_set_increments(GTK_SPIN_BUTTON(w->spin), 0.00001, 0.001);
                               gtk_widget_set_halign(w->spin, GTK_ALIGN_CENTER);
                               gtk_box_append(GTK_BOX(root), w->spin);

                               w->status_label = gtk_label_new("Adjust relative multiplier.");
                               gtk_widget_set_halign(w->status_label, GTK_ALIGN_CENTER);
                               gtk_label_set_wrap(GTK_LABEL(w->status_label), TRUE);
                               gtk_label_set_max_width_chars(GTK_LABEL(w->status_label), 32);
                               gtk_widget_add_css_class(w->status_label, "dim-label");
                               gtk_box_append(GTK_BOX(root), w->status_label);

                               GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                               gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
                               gtk_box_append(GTK_BOX(root), btn_row);

                               GtkWidget *rb = gtk_button_new_with_label("Reset");
                               g_signal_connect(rb, "clicked", G_CALLBACK(on_reset), w);
                               gtk_box_append(GTK_BOX(btn_row), rb);

                               GtkWidget *sb = gtk_button_new_with_label("Resync Baseline");
                               g_signal_connect(sb, "clicked", G_CALLBACK(on_resync), w);
                               gtk_box_append(GTK_BOX(btn_row), sb);

                               GtkWidget *change_default_btn = gtk_button_new_with_label("Change Default ICC Profile");
                               gtk_widget_set_halign(change_default_btn, GTK_ALIGN_CENTER);
                               g_signal_connect(change_default_btn, "clicked", G_CALLBACK(on_change_default_clicked), w);
                               gtk_box_append(GTK_BOX(root), change_default_btn);

                               GtkWidget *reset_all_btn = gtk_button_new_with_label("Reset All Settings & Relaunch");
                               gtk_widget_add_css_class(reset_all_btn, "destructive-action");
                               gtk_widget_set_halign(reset_all_btn, GTK_ALIGN_CENTER);
                               g_signal_connect(reset_all_btn, "clicked", G_CALLBACK(on_reset_all), w);
                               gtk_box_append(GTK_BOX(root), reset_all_btn);

                               g_signal_connect(w->scale,         "change-value",    G_CALLBACK(on_scale_changed),   w);
                               g_signal_connect(w->spin,          "value-changed",   G_CALLBACK(on_spin_changed),    w);
                               g_signal_connect(w->monitor_combo, "notify::selected", G_CALLBACK(on_monitor_changed), w);

                               load_output_into_ui(w, 0);
                               gtk_window_present(GTK_WINDOW(win));
                               do_apply(w);
                               setup_hotplug_monitor(w);
                           }

                           int main(int argc, char *argv[]) {
                               const char *home_env = getenv("HOME");
                               if (home_env) {
                                   char lock_dir[512], lock_path[512];
                                   snprintf(lock_dir,  sizeof(lock_dir),  "%s/.config/gammactrl", home_env);
                                   snprintf(lock_path, sizeof(lock_path), "%s/.config/gammactrl/gammactrl.lock", home_env);
                                   mkdir(lock_dir, 0755);
                                   int lock_fd = open(lock_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
                                   if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
                                       fprintf(stderr, "[gammactrl] Already running.\n");
                                       close(lock_fd);
                                       return 1;
                                   }
                               }
                               AppWidgets w = {0};
                               GtkApplication *app = gtk_application_new("org.gammactrl", G_APPLICATION_DEFAULT_FLAGS);
                               g_signal_connect(app, "activate", G_CALLBACK(activate), &w);
                               int status = g_application_run(G_APPLICATION(app), argc, argv);
                               g_object_unref(app);
                               return status;
                           }
