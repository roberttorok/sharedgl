/*
 * Per-frame profile. See inc/server/frameprof.h, inc/sglprof.h and PROFILING.md.
 *
 *   SGL_FRAMEPROF=1 or a file name     turn it on (1 means sgl_frames.csv in the runtime folder)
 *   SGL_FRAMEPROF_SECONDS=90           how long to record, counted from the first frame
 *
 * Recording starts at the first SwapBuffers and stops by itself, so every run covers the
 * same stretch of a time-based benchmark. Rows are kept in memory and written every few
 * seconds, so the file I/O stays out of the frames being measured.
 */
#define SHAREDGL_HOST
#include <sharedgl.h>
#include <sglprof.h>
#include <sgldebug.h>
#include <server/frameprof.h>
#include <server/processor.h>

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define FP_RUNTIME_DIR ""
#else
#define FP_RUNTIME_DIR "/dev/shm/"
#endif

bool sgl_frameprof_on;
uint64_t sgl_frameprof_host[SGL_FP_COUNT];

struct fp_row {
    uint64_t host_tsc;                       /* end of the frame */
    uint64_t host[SGL_FP_COUNT];
    uint64_t guest_tsc;                      /* guest TSC when SwapBuffers was entered */
    uint32_t guest[SGL_PROF_G_COUNT];        /* this frame's share of each guest counter */
};

static struct sgl_prof_shared *fp_shared;
static char fp_path[1024];
static double fp_seconds = 90.0;
static bool fp_started, fp_finished;
static uint64_t fp_tsc0, fp_counter0;        /* for the TSC rate: taken when the server starts */
static uint64_t fp_first_tsc, fp_last_tsc, fp_last_guest_tsc, fp_last_flush;
static uint32_t fp_last_guest[SGL_PROF_G_COUNT];
static struct fp_row *fp_rows;
static size_t fp_count, fp_capacity, fp_written;
static uint64_t fp_frames;
static uint64_t fp_guest_span, fp_host_span; /* for tsc_ratio */
static uint64_t fp_cmd_count[SGL_CMD_MAX], fp_cmd_cycles[SGL_CMD_MAX];

static double fp_tsc_hz(void)
{
    uint64_t counter = SDL_GetPerformanceCounter() - fp_counter0;
    double seconds = (double)counter / (double)SDL_GetPerformanceFrequency();
    return seconds > 0.0 ? (double)(sgl_tsc() - fp_tsc0) / seconds : 1.0;
}

void sgl_frameprof_init(void *shared)
{
    const char *value = getenv("SGL_FRAMEPROF");
    const char *seconds = getenv("SGL_FRAMEPROF_SECONDS");

    fp_shared = (struct sgl_prof_shared *)((char *)shared + SGL_PROF_OFFSET);
    memset((void *)fp_shared, 0, sizeof(*fp_shared));
    if (value == NULL || value[0] == 0 || strcmp(value, "0") == 0)
        return;

    if (strcmp(value, "1") == 0)
        snprintf(fp_path, sizeof(fp_path), "%ssgl_frames.csv", FP_RUNTIME_DIR);
    else
        snprintf(fp_path, sizeof(fp_path), "%s", value);
    if (seconds != NULL && atof(seconds) > 0)
        fp_seconds = atof(seconds);

    fp_tsc0 = sgl_tsc();
    fp_counter0 = SDL_GetPerformanceCounter();
    if (getenv("SGL_FRAMEPROF_CONTROL") != NULL)
        fp_shared->control = (uint32_t)strtoul(getenv("SGL_FRAMEPROF_CONTROL"), NULL, 0);
    fp_shared->version = SGL_PROF_VERSION;
    fp_shared->magic = SGL_PROF_MAGIC;
    sgl_frameprof_on = true;
    PRINT_LOG("frame profile: recording %.0f s from the first frame into %s\n", fp_seconds, fp_path);
}

void sgl_frameprof_command(int cmd, uint64_t cycles)
{
    if (cmd >= 0 && cmd < SGL_CMD_MAX) {
        fp_cmd_count[cmd]++;
        fp_cmd_cycles[cmd] += cycles;
    }
}

static const char *fp_crt(void)
{
#if defined(_UCRT)
    return "ucrt";
#elif defined(_WIN32)
    return "msvcrt";
#elif defined(__GLIBC__)
    return "glibc";
#else
    return "unknown";
#endif
}

static void fp_write_header(FILE *out)
{
    time_t now = time(NULL);
    char date[64];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(out, "# sglprof %d\n", SGL_PROF_VERSION);
    fprintf(out, "# started=%s\n", date);
#ifdef _WIN32
    fprintf(out, "# host_os=Windows\n");
#else
    fprintf(out, "# host_os=Linux\n");
#endif
    fprintf(out, "# compiler=%s crt=%s\n", __VERSION__, fp_crt());
    fprintf(out, "# gl_vendor=%s\n", (const char *)glGetString(GL_VENDOR));
    fprintf(out, "# gl_renderer=%s\n", (const char *)glGetString(GL_RENDERER));
    fprintf(out, "# gl_version=%s\n", (const char *)glGetString(GL_VERSION));
    fprintf(out, "# guest_version=%u guest_rdtsc_cycles=%u\n", fp_shared->guest_version, fp_shared->guest_rdtsc);
    fprintf(out, "# record_seconds=%.0f\n", fp_seconds);
    fprintf(out, "# control=%u\n", fp_shared->control);
    {
        static const char *names[] = { "SGL_PROFILE", "SGL_FRAMETIME", "SGL_DRAWCHECK", "SGL_SHADERDUMP" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (getenv(names[i]) != NULL)
                fprintf(out, "# env %s=%s\n", names[i], getenv(names[i]));
    }
    fprintf(out, "frame,t_ms,host_frame_us,h_wait_us,h_setup_us,h_exec_us,h_errcheck_us,h_readback_us,h_other_us,"
                 "submits,commands,kbytes,h_sleeps,"
                 "guest_frame_us,g_wait_us,g_lock_us,g_copy_us,g_swapcopy_us,g_pace_us,g_present_us,"
                 "g_submits,g_kbytes,g_sleeps,g_presents\n");
}

static void fp_flush(bool final)
{
    double hz = fp_tsc_hz(), us = 1e6 / hz;
    FILE *out = fopen(fp_path, fp_written == 0 ? "w" : "a");

    if (out == NULL) {
        PRINT_LOG("frame profile: cannot write %s\n", fp_path);
        fp_count = 0;
        return;
    }
    if (fp_written == 0)
        fp_write_header(out);

    for (size_t i = 0; i < fp_count; i++) {
        struct fp_row *r = &fp_rows[i];
        uint64_t previous = i > 0 ? fp_rows[i - 1].host_tsc : fp_last_flush; /* fp_last_flush starts at the first frame */
        double frame = (double)(r->host_tsc - previous) * us;
        double busy = 0;
        for (int k = SGL_FP_WAIT; k <= SGL_FP_READBACK; k++)
            busy += (double)r->host[k] * us;
        fprintf(out, "%llu,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%llu,%llu,%.1f,%llu,"
                     "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%.1f,%u,%u\n",
            (unsigned long long)(fp_written + i + 1),
            (double)(r->host_tsc - fp_first_tsc) * us / 1000.0,
            frame,
            r->host[SGL_FP_WAIT] * us, r->host[SGL_FP_SETUP] * us, r->host[SGL_FP_EXEC] * us,
            r->host[SGL_FP_ERRCHECK] * us, r->host[SGL_FP_READBACK] * us, frame - busy,
            (unsigned long long)r->host[SGL_FP_SUBMITS], (unsigned long long)r->host[SGL_FP_COMMANDS],
            r->host[SGL_FP_BYTES] / 1024.0, (unsigned long long)r->host[SGL_FP_SLEEPS],
            (double)r->guest_tsc * us,
            r->guest[SGL_PROF_G_SUBMIT_WAIT] * us, r->guest[SGL_PROF_G_SUBMIT_LOCK] * us,
            r->guest[SGL_PROF_G_SUBMIT_COPY] * us, r->guest[SGL_PROF_G_SWAP_COPY] * us,
            r->guest[SGL_PROF_G_PACE] * us, r->guest[SGL_PROF_G_PRESENT_THREAD] * us,
            r->guest[SGL_PROF_G_SUBMITS], r->guest[SGL_PROF_G_BYTES] / 1024.0,
            r->guest[SGL_PROF_G_SLEEPS], r->guest[SGL_PROF_G_PRESENTS]);
    }
    if (fp_count > 0)
        fp_last_flush = fp_rows[fp_count - 1].host_tsc;
    fp_written += fp_count;
    fp_count = 0;

    if (final) {
        fprintf(out, "# frames=%llu seconds=%.3f tsc_mhz=%.3f tsc_ratio_guest_over_host=%.5f guest_rdtsc_cycles=%u\n",
            (unsigned long long)fp_written, (double)(fp_last_tsc - fp_first_tsc) / hz, hz / 1e6,
            fp_host_span ? (double)fp_guest_span / (double)fp_host_span : 0.0, fp_shared->guest_rdtsc);
    }
    fclose(out);

    if (final) {
        /* per-command totals over the whole recording */
        char path[1100];
        snprintf(path, sizeof(path), "%s.commands.csv", fp_path);
        out = fopen(path, "w");
        if (out != NULL) {
            fprintf(out, "command,count,total_us,us_per_frame,ns_per_call\n");
            for (int i = 0; i < SGL_CMD_MAX; i++)
                if (fp_cmd_count[i])
                    fprintf(out, "%s,%llu,%.1f,%.2f,%.1f\n", sgl_cmd2str(i), (unsigned long long)fp_cmd_count[i],
                        fp_cmd_cycles[i] * us, fp_cmd_cycles[i] * us / (double)(fp_written ? fp_written : 1),
                        fp_cmd_cycles[i] * us * 1000.0 / (double)fp_cmd_count[i]);
            fclose(out);
        }
    }
}

void sgl_frameprof_swap(void *shared)
{
    uint64_t now = sgl_tsc();
    uint64_t guest_tsc;
    uint32_t guest[SGL_PROF_G_COUNT];
    (void)shared;

    if (!sgl_frameprof_on || fp_finished)
        return;

    guest_tsc = ((uint64_t)fp_shared->swap_tsc_hi << 32) | fp_shared->swap_tsc_lo;
    for (int i = 0; i < SGL_PROF_G_COUNT; i++)
        guest[i] = fp_shared->counter[i];

    if (!fp_started) {
        /* the first frame only sets the starting point */
        fp_started = true;
        fp_first_tsc = fp_last_tsc = fp_last_flush = now;
        fp_last_guest_tsc = guest_tsc;
        memcpy(fp_last_guest, guest, sizeof(guest));
        memset(sgl_frameprof_host, 0, sizeof(sgl_frameprof_host));
        memset(fp_cmd_count, 0, sizeof(fp_cmd_count));
        memset(fp_cmd_cycles, 0, sizeof(fp_cmd_cycles));
        return;
    }

    if (fp_count == fp_capacity) {
        size_t capacity = fp_capacity ? fp_capacity * 2 : 4096;
        struct fp_row *rows = realloc(fp_rows, capacity * sizeof(*rows));
        if (rows == NULL)
            return;
        fp_rows = rows;
        fp_capacity = capacity;
    }

    struct fp_row *r = &fp_rows[fp_count++];
    r->host_tsc = now;
    memcpy(r->host, sgl_frameprof_host, sizeof(r->host));
    r->guest_tsc = guest_tsc - fp_last_guest_tsc;
    for (int i = 0; i < SGL_PROF_G_COUNT; i++)
        r->guest[i] = guest[i] - fp_last_guest[i];

    fp_guest_span += r->guest_tsc;
    fp_host_span += now - fp_last_tsc;
    fp_last_tsc = now;
    fp_last_guest_tsc = guest_tsc;
    memcpy(fp_last_guest, guest, sizeof(guest));
    memset(sgl_frameprof_host, 0, sizeof(sgl_frameprof_host));
    fp_frames++;

    double hz = fp_tsc_hz();
    if ((double)(now - fp_first_tsc) >= fp_seconds * hz) {
        fp_flush(true);
        fp_finished = true;
        sgl_frameprof_on = false;
        fp_shared->magic = 0;
        PRINT_LOG("frame profile: done, %llu frames in %s\n", (unsigned long long)fp_written, fp_path);
    }
    else if ((double)(now - fp_rows[0].host_tsc) >= 5.0 * hz) {
        fp_flush(false);
    }
}
