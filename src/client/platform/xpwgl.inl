/*
 * Windows XP front end: the client as a drop-in opengl32.dll.
 *
 * XP's own opengl32.dll asks the display driver which ICD to load, through an
 * escape that the QXL driver does not implement, and never reads the MSOGL
 * registry key that newer Windows versions honour. So on XP the client has to
 * BE opengl32.dll and export the wgl* entry points the system one would.
 * GDI forwards ChoosePixelFormat, SetPixelFormat, DescribePixelFormat,
 * GetPixelFormat and SwapBuffers to these, too.
 *
 * Included from windrv.c, for its pixel format table and context list.
 */

#define XPWGL_MAX_WINDOWS 256
#include <intrin.h>
extern void SGL_INTERNAL glimpl_profile_record(int index, unsigned long long cycles);
#include <sglprof.h>
extern struct sgl_prof_shared * SGL_INTERNAL glimpl_prof(void);

struct xpwgl_window_format {
    HWND hwnd;
    int format;
};

static struct xpwgl_window_format xpwgl_formats[XPWGL_MAX_WINDOWS];
static DWORD xpwgl_tls_context = TLS_OUT_OF_INDEXES;
static DWORD xpwgl_tls_dc = TLS_OUT_OF_INDEXES;
static LONG xpwgl_tls_once = 0;
static int xpwgl_swap_interval = -1; /* -1: the application never asked; reported back only */

static void xpwgl_log(const char *message)
{
    char path[MAX_PATH];
    DWORD length = GetTempPathA(sizeof(path) - 16, path);
    HANDLE file;
    DWORD written;

    OutputDebugStringA(message);
    if (length == 0)
        return;
    strcat(path, "sharedgl.log");
    file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, message, (DWORD)strlen(message), &written, NULL);
        WriteFile(file, "\r\n", 2, &written, NULL);
        CloseHandle(file);
    }
}

static void xpwgl_tls_init(void)
{
    if (InterlockedExchange(&xpwgl_tls_once, 1) == 0) {
        xpwgl_tls_context = TlsAlloc();
        xpwgl_tls_dc = TlsAlloc();
    }
}

static BOOL xpwgl_client_size(HDC hdc, int *width, int *height)
{
    HWND window = WindowFromDC(hdc);
    RECT rect;

    if (window == NULL || !GetClientRect(window, &rect))
        return FALSE;

    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
    return TRUE;
}

static struct xpwgl_window_format *xpwgl_find_format(HWND hwnd, BOOL create)
{
    struct xpwgl_window_format *free_slot = NULL;

    for (int i = 0; i < XPWGL_MAX_WINDOWS; i++) {
        if (xpwgl_formats[i].hwnd == hwnd)
            return &xpwgl_formats[i];
        /* windows that no longer exist give their slot back */
        if (free_slot == NULL && (xpwgl_formats[i].hwnd == NULL || (create && !IsWindow(xpwgl_formats[i].hwnd))))
            free_slot = &xpwgl_formats[i];
    }

    if (create && free_slot != NULL) {
        free_slot->hwnd = hwnd;
        free_slot->format = 0;
        return free_slot;
    }
    return NULL;
}

/*
 * The server's frame is bottom-up, which is exactly a bottom-up DIB, so showing
 * it needs no flip on either side. With SGL_WINED3D_DONT_VFLIP (do_vflip = FALSE)
 * the image is the other way up.
 *
 * Reading a frame back takes under a millisecond, but pushing it through GDI and
 * the QXL driver takes 3 to 15. So the application does not wait for that: frames
 * go into a triple buffer and a thread shows the newest one. An application
 * drawing faster than the display path gets real rendering speed, with frames
 * skipped on screen rather than the application throttled. The last frame is
 * always shown. SGL_SYNC_PRESENT=true blits in the application's thread instead.
 */
struct xpwgl_frame {
    void *pixels;
    HWND window;
    int width, height;
};

static struct xpwgl_frame xpwgl_frames[3];
static struct xpwgl_frame *xpwgl_back, *xpwgl_ready, *xpwgl_front;
static CRITICAL_SECTION xpwgl_present_lock;
static HANDLE xpwgl_present_event;
static BOOL xpwgl_ready_full = FALSE;
static int xpwgl_present_mode = -1; /* -1 undecided, 0 sync, 1 threaded */
static volatile LONG xpwgl_stat_rendered, xpwgl_stat_shown;

static void xpwgl_blit(HDC hdc, const void *pixels, int stride_pixels, int width, int height)
{
    BITMAPINFOHEADER header;

    memset(&header, 0, sizeof(header));
    header.biSize = sizeof(header);
    header.biWidth = stride_pixels;
    header.biHeight = do_vflip ? height : -height;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, 0, 0, width, height, 0, 0, 0, height, pixels, (BITMAPINFO*)&header, DIB_RGB_COLORS);
}

static DWORD WINAPI xpwgl_present_thread(LPVOID unused)
{
    (void)unused;

    for (;;) {
        struct xpwgl_frame *frame;
        HDC hdc;

        WaitForSingleObject(xpwgl_present_event, INFINITE);

        for (;;) {
            EnterCriticalSection(&xpwgl_present_lock);
            if (!xpwgl_ready_full) {
                LeaveCriticalSection(&xpwgl_present_lock);
                break;
            }
            frame = xpwgl_ready;
            xpwgl_ready = xpwgl_front;
            xpwgl_front = frame;
            xpwgl_ready_full = FALSE;
            LeaveCriticalSection(&xpwgl_present_lock);

            /* a DC of its own: GDI does not allow one DC in two threads at once */
            unsigned long long prof_begin = __rdtsc();
            struct sgl_prof_shared *control = glimpl_prof();
            hdc = control && (control->control & SGL_PROF_CTL_NO_BLIT) ? NULL : GetDC(frame->window);
            if (hdc != NULL) {
                xpwgl_blit(hdc, frame->pixels, frame->width, frame->width, frame->height);
                ReleaseDC(frame->window, hdc);
                InterlockedIncrement(&xpwgl_stat_shown);
            }
            {
                struct sgl_prof_shared *prof = glimpl_prof();
                if (prof) {
                    prof->counter[SGL_PROF_G_PRESENT_THREAD] += (uint32_t)(__rdtsc() - prof_begin);
                    prof->counter[SGL_PROF_G_PRESENTS] += 1;
                }
            }
        }
    }
    return 0;
}

static BOOL xpwgl_present_start(void)
{
    char value[8] = "";
    size_t size = (size_t)max_width * max_height * 4;
    HANDLE thread;

    if (GetEnvironmentVariableA("SGL_SYNC_PRESENT", value, sizeof(value)) > 0 && strcmp(value, "true") == 0)
        return FALSE;

    for (int i = 0; i < 3; i++) {
        xpwgl_frames[i].pixels = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (xpwgl_frames[i].pixels == NULL)
            return FALSE;
    }

    xpwgl_back = &xpwgl_frames[0];
    xpwgl_ready = &xpwgl_frames[1];
    xpwgl_front = &xpwgl_frames[2];
    InitializeCriticalSection(&xpwgl_present_lock);
    xpwgl_present_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    thread = CreateThread(NULL, 64 * 1024, xpwgl_present_thread, NULL, 0, NULL);
    if (thread == NULL)
        return FALSE;
    CloseHandle(thread);
    return TRUE;
}

static void xpwgl_stats(void)
{
    static int stats = -1;
    static DWORD last;
    char line[96];
    DWORD now;

    if (stats < 0) {
        char value[8] = "";
        stats = GetEnvironmentVariableA("SGL_STATS", value, sizeof(value)) > 0;
        last = GetTickCount();
    }
    if (!stats)
        return;

    InterlockedIncrement(&xpwgl_stat_rendered);
    now = GetTickCount();
    if (now - last >= 5000) {
        sprintf(line, "rendered %.1f FPS, shown %.1f FPS",
            InterlockedExchange(&xpwgl_stat_rendered, 0) * 1000.0 / (now - last),
            InterlockedExchange(&xpwgl_stat_shown, 0) * 1000.0 / (now - last));
        xpwgl_log(line);
        last = now;
    }
}

/*
 * Rendering faster than anything can show only heats the GPU and CPU: every extra
 * frame costs a full round trip to the host and a readback. The host alone sets the
 * limit (sglrenderer -f, default 60, 0 = none) and publishes it in shared memory; it
 * is read every frame. The application has no say: its swap interval is ignored, and
 * nothing in the guest's environment is consulted. A server too old to publish a
 * limit gets the default.
 */
static void xpwgl_pace(void)
{
    static BOOL started = FALSE;
    static LARGE_INTEGER frequency, next;
    LARGE_INTEGER now;
    LONGLONG period;
    int cap, fps;

    if (!started) {
        QueryPerformanceFrequency(&frequency);
        timeBeginPeriod(1); /* or XP's Sleep() only wakes every 15.6 ms */
        started = TRUE;
    }

    cap = *(volatile int*)pb_global_ptr(SGL_OFFSET_REGISTER_FPS_CAP);
    fps = (cap & SGL_FPS_CAP_SET) ? (cap & SGL_FPS_CAP_MASK) : SGL_FPS_CAP_DEFAULT;
    if (fps <= 0) {
        next.QuadPart = 0;
        return;
    }

    period = frequency.QuadPart / fps;
    QueryPerformanceCounter(&now);

    /* A late frame has already spent its budget rendering. Do not add a
     * second period before the next frame can be presented. */
    if (next.QuadPart == 0 || now.QuadPart >= next.QuadPart) {
        next.QuadPart = now.QuadPart + period;
        return;
    }

    for (;;) {
        LONGLONG remaining_ms;
        QueryPerformanceCounter(&now);
        remaining_ms = (next.QuadPart - now.QuadPart) * 1000 / frequency.QuadPart;
        if (now.QuadPart >= next.QuadPart)
            break;
        if (remaining_ms >= 2)
            Sleep((DWORD)remaining_ms - 1);
        else
            spin_pause();
    }
    next.QuadPart = now.QuadPart + period;
}

static BOOL xp_present(HDC hdc)
{
    unsigned long long prof_begin = __rdtsc(), prof_pace, pace_ticks = 0;
    BOOL paced = FALSE;
    HWND window = WindowFromDC(hdc);
    const char *source;
    int width = real_width, height = real_height;

    if (!glimpl_is_ready())
        return FALSE;

    xpwgl_client_size(hdc, &width, &height);
    if (width > max_width)
        width = max_width;
    if (height > max_height)
        height = max_height;

    /* a minimized window still has to deliver its commands */
    if (width <= 0 || height <= 0) {
        glFlush();
        return TRUE;
    }

    if (xpwgl_present_mode < 0)
        xpwgl_present_mode = xpwgl_present_start() ? 1 : 0;

    /* the framebuffer belongs to all clients, so it is only safe under this lock */
    swap_sync_lock = pb_ptr(SGL_OFFSET_REGISTER_SWAP_BUFFERS_SYNC);
    source = glimpl_fb_address();

    struct sgl_prof_shared *frame_prof = glimpl_prof();
    unsigned long long prof_swapped = 0;
    if (frame_prof) {
        /* the server reads this when it handles the swap below: one guest frame per swap */
        frame_prof->swap_tsc_lo = (uint32_t)prof_begin;
        frame_prof->swap_tsc_hi = (uint32_t)(prof_begin >> 32);
    }

    spin_lock(swap_sync_lock);
    glimpl_swap_buffers(width, height, 0, GL_BGRA);
    if (frame_prof) prof_swapped = __rdtsc();

    if (xpwgl_present_mode == 0 || window == NULL) {
        if (!frame_prof || !(frame_prof->control & SGL_PROF_CTL_NO_BLIT))
            xpwgl_blit(hdc, source, max_width, width, height);
        spin_unlock(swap_sync_lock);
    }
    else {
        struct xpwgl_frame *frame = xpwgl_back;
        char *target = frame->pixels;

        for (int y = 0; y < height; y++)
            memcpy(target + (size_t)y * width * 4, source + (size_t)y * max_width * 4, (size_t)width * 4);
        spin_unlock(swap_sync_lock);

        frame->window = window;
        frame->width = width;
        frame->height = height;

        /*
         * Wait for the frame's slot before showing it, not after. Waiting after
         * spaces out the starts of frames, and each one then appears whenever its
         * rendering happens to finish: a few ms early or late every frame, which
         * shows as judder and double images in motion. Waiting here shows frames
         * at even intervals, and the next one starts right after, also evenly.
         */
        prof_pace = __rdtsc();
        xpwgl_pace();
        pace_ticks = __rdtsc() - prof_pace;
        paced = TRUE;

        EnterCriticalSection(&xpwgl_present_lock);
        xpwgl_back = xpwgl_ready;
        xpwgl_ready = frame;
        xpwgl_ready_full = TRUE;
        LeaveCriticalSection(&xpwgl_present_lock);
        SetEvent(xpwgl_present_event);
    }

    xpwgl_stats();
    if (!paced) {
        /* the synchronous blit has shown the frame already */
        prof_pace = __rdtsc();
        xpwgl_pace();
        pace_ticks = __rdtsc() - prof_pace;
    }
    glimpl_profile_record(2, __rdtsc() - prof_begin);
    glimpl_profile_record(3, 1);
    glimpl_profile_record(4, pace_ticks);
    if (frame_prof) {
        frame_prof->counter[SGL_PROF_G_SWAP_COPY] += (uint32_t)(prof_pace - prof_swapped);
        frame_prof->counter[SGL_PROF_G_PACE] += (uint32_t)pace_ticks;
    }
    return TRUE;
}

/*
 * With SGL_STATS set, a crash writes its address and the code addresses found on
 * the stack to sharedgl.log. Windows XP has no other record of these.
 */
static LONG WINAPI xpwgl_crash_logger(PEXCEPTION_POINTERS info)
{
    static LONG once = 0;
    char line[320];
    DWORD *stack = (DWORD*)info->ContextRecord->Esp;
    int found = 0;

    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || InterlockedExchange(&once, 1))
        return EXCEPTION_CONTINUE_SEARCH;

    sprintf(line, "CRASH eip=%08lx thread=%lu", info->ContextRecord->Eip, GetCurrentThreadId());
    xpwgl_log(line);
    for (int i = 0; i < 2048 && found < 16 && !IsBadReadPtr(stack + i, 4); i++) {
        MEMORY_BASIC_INFORMATION mbi;
        char module[MAX_PATH];
        if (VirtualQuery((void*)stack[i], &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE && (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_WRITECOPY | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE))
            && GetModuleFileNameA((HMODULE)mbi.AllocationBase, module, sizeof(module))) {
            const char *name = strrchr(module, '\\');
            sprintf(line, "  [esp+%x] %s+%lx", i * 4, name ? name + 1 : module, stack[i] - (DWORD)mbi.AllocationBase);
            xpwgl_log(line);
            found++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void xpwgl_install_crash_logger(void)
{
    char value[8] = "";
    if (GetEnvironmentVariableA("SGL_STATS", value, sizeof(value)) > 0)
        AddVectoredExceptionHandler(1, xpwgl_crash_logger);
}

/*
 * The XP QXL driver accepts 16-bit display modes, but QEMU then shows the 16-bit
 * framebuffer as if it were 32-bit: two half-height copies in wrong colours, for
 * everything on screen. Frames from the server are 32-bit and GDI converts them
 * anyway, so nothing is gained from a 16-bit desktop. Keep the real mode at 32 bits
 * for processes using this DLL; the game's own surfaces can still be 16-bit.
 * SGL_ALLOW_16BPP=true leaves the requests alone.
 *
 * ChangeDisplaySettings is hooked in user32 itself, because Direct3D wrappers can
 * call it before any context exists. XP SP2 and later have hot-patch space there:
 * five spare bytes, then a two-byte no-op.
 */
typedef LONG (WINAPI *xpwgl_cdsexa_t)(LPCSTR, LPDEVMODEA, HWND, DWORD, LPVOID);
typedef LONG (WINAPI *xpwgl_cdsexw_t)(LPCWSTR, LPDEVMODEW, HWND, DWORD, LPVOID);
static xpwgl_cdsexa_t xpwgl_real_cdsexa;
static xpwgl_cdsexw_t xpwgl_real_cdsexw;

static LONG WINAPI xpwgl_cdsexa(LPCSTR device, LPDEVMODEA mode, HWND hwnd, DWORD flags, LPVOID param)
{
    if (mode != NULL && (mode->dmFields & DM_BITSPERPEL) && mode->dmBitsPerPel > 0 && mode->dmBitsPerPel < 32) {
        DEVMODEA copy;
        memset(&copy, 0, sizeof(copy));
        memcpy(&copy, mode, mode->dmSize < sizeof(copy) ? mode->dmSize : sizeof(copy));
        copy.dmSize = sizeof(copy);
        copy.dmDriverExtra = 0;
        copy.dmBitsPerPel = 32;
        return xpwgl_real_cdsexa(device, &copy, hwnd, flags, param);
    }
    return xpwgl_real_cdsexa(device, mode, hwnd, flags, param);
}

static LONG WINAPI xpwgl_cdsexw(LPCWSTR device, LPDEVMODEW mode, HWND hwnd, DWORD flags, LPVOID param)
{
    if (mode != NULL && (mode->dmFields & DM_BITSPERPEL) && mode->dmBitsPerPel > 0 && mode->dmBitsPerPel < 32) {
        DEVMODEW copy;
        memset(&copy, 0, sizeof(copy));
        memcpy(&copy, mode, mode->dmSize < sizeof(copy) ? mode->dmSize : sizeof(copy));
        copy.dmSize = sizeof(copy);
        copy.dmDriverExtra = 0;
        copy.dmBitsPerPel = 32;
        return xpwgl_real_cdsexw(device, &copy, hwnd, flags, param);
    }
    return xpwgl_real_cdsexw(device, mode, hwnd, flags, param);
}

static void *xpwgl_hot_patch(void *target, void *hook)
{
    unsigned char *code = target;
    DWORD old;

    /* mov edi, edi, with padding before it */
    if (code == NULL || code[0] != 0x8B || code[1] != 0xFF)
        return NULL;
    for (int i = 1; i <= 5; i++)
        if (code[-i] != 0x90 && code[-i] != 0xCC)
            return NULL;
    if (!VirtualProtect(code - 5, 7, PAGE_EXECUTE_READWRITE, &old))
        return NULL;

    code[-5] = 0xE9; /* jmp hook */
    *(DWORD*)(code - 4) = (DWORD)((unsigned char*)hook - code);
    *(WORD*)code = 0xF9EB; /* jmp short back to it, written last and atomically */
    VirtualProtect(code - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code - 5, 7);
    return code + 2; /* the original function, past the no-op */
}

void windrv_install_display_hook(void)
{
    char value[8] = "";
    HMODULE user32 = LoadLibraryA("user32.dll");

    if (GetEnvironmentVariableA("SGL_ALLOW_16BPP", value, sizeof(value)) > 0 && strcmp(value, "true") == 0)
        return;

    xpwgl_real_cdsexa = xpwgl_hot_patch((void*)GetProcAddress(user32, "ChangeDisplaySettingsExA"), xpwgl_cdsexa);
    xpwgl_real_cdsexw = xpwgl_hot_patch((void*)GetProcAddress(user32, "ChangeDisplaySettingsExW"), xpwgl_cdsexw);
    if (xpwgl_real_cdsexa == NULL || xpwgl_real_cdsexw == NULL)
        xpwgl_log("SharedGL: could not hook ChangeDisplaySettingsEx; 16-bit modes will display wrongly");
}

/* With SGL_STATS set, names each unimplemented GL function the first time it is called */
void glimpl_note_stub(const char *name)
{
    static const char *seen[128];
    static int count = 0, stats = -1;
    char line[128];

    if (stats < 0) {
        char value[8] = "";
        stats = GetEnvironmentVariableA("SGL_STATS", value, sizeof(value)) > 0;
    }
    if (!stats)
        return;
    for (int i = 0; i < count; i++)
        if (seen[i] == name)
            return;
    if (count < 128)
        seen[count++] = name;
    sprintf(line, "stub called: %s", name);
    xpwgl_log(line);
}

/* ---------------- pixel formats ---------------- */

static int xpwgl_score_format(const PIXELFORMATDESCRIPTOR *have, const PIXELFORMATDESCRIPTOR *want)
{
    int score = 0;

    if (!(want->dwFlags & PFD_DOUBLEBUFFER_DONTCARE)) {
        if ((want->dwFlags & PFD_DOUBLEBUFFER) != (have->dwFlags & PFD_DOUBLEBUFFER))
            score += 100000;
    }

    /* anything asked for must exist; more than asked for costs a little */
    if (have->cAlphaBits < want->cAlphaBits)
        score += 20000;
    if (have->cDepthBits < want->cDepthBits)
        score += 40000;
    if (have->cStencilBits < want->cStencilBits)
        score += 40000;
    if (have->cAccumBits < want->cAccumBits)
        score += 10000;

    score += abs((int)have->cDepthBits - (int)want->cDepthBits) * 10;
    score += abs((int)have->cStencilBits - (int)want->cStencilBits) * 10;
    score += abs((int)have->cAlphaBits - (int)want->cAlphaBits) * 10;
    score += abs((int)have->cAccumBits - (int)want->cAccumBits);
    return score;
}

int WINAPI wglChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *want)
{
    int best = 0, best_score = 0x7fffffff;

    if (want == NULL)
        return 0;
    if (!pfd_count)
        pfd_init();

    for (int i = 0; i < pfd_count; i++) {
        /* BGR formats exist for the ARB interface; plain requests want the native order */
        int score;
        if (pfd_table[i].cRedShift != 16)
            continue;
        score = xpwgl_score_format(&pfd_table[i], want);
        if (score < best_score) {
            best_score = score;
            best = i + 1;
        }
    }
    (void)hdc;
    return best;
}

int WINAPI wglDescribePixelFormat(HDC hdc, int format, UINT bytes, PIXELFORMATDESCRIPTOR *pfd)
{
    if (!pfd_count)
        pfd_init();

    if (pfd == NULL)
        return pfd_count;
    if (format < 1 || format > pfd_count || bytes < sizeof(PIXELFORMATDESCRIPTOR)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    memcpy(pfd, &pfd_table[format - 1], sizeof(PIXELFORMATDESCRIPTOR));
    (void)hdc;
    return pfd_count;
}

int WINAPI wglGetPixelFormat(HDC hdc)
{
    struct xpwgl_window_format *entry = xpwgl_find_format(WindowFromDC(hdc), FALSE);
    return entry ? entry->format : 0;
}

BOOL WINAPI wglSetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *pfd)
{
    struct xpwgl_window_format *entry;
    HWND window = WindowFromDC(hdc);

    (void)pfd;
    if (!pfd_count)
        pfd_init();
    if (format < 1 || format > pfd_count || window == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    entry = xpwgl_find_format(window, TRUE);
    if (entry == NULL)
        return FALSE;
    /* a window's pixel format can only be set once */
    if (entry->format != 0 && entry->format != format)
        return FALSE;
    entry->format = format;
    return TRUE;
}

BOOL WINAPI wglSwapBuffers(HDC hdc)
{
    return xp_present(hdc);
}

BOOL WINAPI wglSwapLayerBuffers(HDC hdc, UINT planes)
{
    if (planes & WGL_SWAP_MAIN_PLANE)
        return xp_present(hdc);
    return FALSE;
}

/* ---------------- contexts ---------------- */

/*
 * The server gives each process one context, so every HGLRC here is a name for
 * the same GL state. Lists and textures are shared automatically, which is what
 * wglShareLists asks for, but separate contexts do not have separate state.
 */
HGLRC WINAPI wglCreateContext(HDC hdc)
{
    struct windrv_context_state *context;

    if (!glimpl_is_ready()) {
        xpwgl_install_crash_logger();
        glimpl_init();
        if (!glimpl_is_ready()) {
            xpwgl_log("SharedGL: cannot reach the server. Is the driver installed, and is sglrenderer running on the host?");
            SetLastError(ERROR_DEVICE_NOT_CONNECTED);
            return NULL;
        }
    }

    context = windrv_alloc_context(hdc, PFD_MAIN_PLANE);
    return context ? (HGLRC)context->handle : NULL;
}

HGLRC WINAPI wglCreateLayerContext(HDC hdc, int plane)
{
    return plane == 0 ? wglCreateContext(hdc) : NULL;
}

BOOL WINAPI wglDeleteContext(HGLRC context)
{
    xpwgl_tls_init();
    if ((HGLRC)TlsGetValue(xpwgl_tls_context) == context) {
        TlsSetValue(xpwgl_tls_context, NULL);
        TlsSetValue(xpwgl_tls_dc, NULL);
    }
    return windrv_free_context((DHGLRC)context);
}

BOOL WINAPI wglMakeCurrent(HDC hdc, HGLRC context)
{
    struct windrv_context_state *state;
    int width, height;

    xpwgl_tls_init();

    if (context == NULL) {
        TlsSetValue(xpwgl_tls_context, NULL);
        TlsSetValue(xpwgl_tls_dc, NULL);
        return TRUE;
    }

    state = windrv_lookup_context((DHGLRC)context);
    if (state == NULL || hdc == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    TlsSetValue(xpwgl_tls_context, context);
    TlsSetValue(xpwgl_tls_dc, hdc);

    /*
     * The first time a context is made current, GL sets the viewport to the
     * window. The server's drawable is max-resolution sized, so do it here.
     */
    if (state->layer_plane == PFD_MAIN_PLANE && !state->viewport_set && xpwgl_client_size(hdc, &width, &height)) {
        state->viewport_set = TRUE;
        glViewport(0, 0, width, height);
        glScissor(0, 0, width, height);
    }
    return TRUE;
}

HGLRC WINAPI wglGetCurrentContext(void)
{
    xpwgl_tls_init();
    return (HGLRC)TlsGetValue(xpwgl_tls_context);
}

HDC WINAPI wglGetCurrentDC(void)
{
    xpwgl_tls_init();
    return (HDC)TlsGetValue(xpwgl_tls_dc);
}

BOOL WINAPI wglCopyContext(HGLRC source, HGLRC destination, UINT mask)
{
    (void)source;
    (void)destination;
    (void)mask;
    return TRUE;
}

BOOL WINAPI wglShareLists(HGLRC first, HGLRC second)
{
    (void)first;
    (void)second;
    return TRUE;
}

/* ---------------- extensions ---------------- */

static const char *wgl_extensions_ext = "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_ARB_pixel_format WGL_ARB_create_context WGL_ARB_create_context_profile WGL_EXT_swap_control";

static const char WINAPI *wglGetExtensionsStringEXT(void)
{
    return wgl_extensions_ext;
}

static const char WINAPI *wglGetExtensionsStringARB_xp(HDC hdc)
{
    (void)hdc;
    return wgl_extensions_ext;
}

/*
 * Presentation is a blit in the guest, so there is no real vsync. The interval is
 * remembered for wglGetSwapIntervalEXT only: the frame rate is the host's decision,
 * see xpwgl_pace().
 */
static BOOL WINAPI wglSwapIntervalEXT(int interval)
{
    xpwgl_swap_interval = interval;
    return TRUE;
}

static int WINAPI wglGetSwapIntervalEXT(void)
{
    return xpwgl_swap_interval < 0 ? 0 : xpwgl_swap_interval;
}

PROC WINAPI wglGetProcAddress(LPCSTR name)
{
    if (name == NULL)
        return NULL;
    if (strcmp(name, "wglGetExtensionsStringEXT") == 0)
        return (PROC)wglGetExtensionsStringEXT;
    if (strcmp(name, "wglGetExtensionsStringARB") == 0)
        return (PROC)wglGetExtensionsStringARB_xp;
    if (strcmp(name, "wglSwapIntervalEXT") == 0)
        return (PROC)wglSwapIntervalEXT;
    if (strcmp(name, "wglGetSwapIntervalEXT") == 0)
        return (PROC)wglGetSwapIntervalEXT;

    PROC proc = DrvGetProcAddress(name);
    if (proc == NULL) {
        /*
         * The server advertises the host's extensions, but only some suffixed
         * names exist here. For promoted extensions the core function is the
         * same call, so hand that out instead of a NULL the caller will jump to.
         */
        size_t length = strlen(name);
        if (length > 3 && length < 128 && name[0] == 'g' && name[1] == 'l' &&
            (strcmp(name + length - 3, "ARB") == 0 || strcmp(name + length - 3, "EXT") == 0)) {
            char core[128];
            memcpy(core, name, length - 3);
            core[length - 3] = 0;
            proc = GetProcAddress(module_handle, core);
        }
    }
    return proc;
}

PROC WINAPI wglGetDefaultProcAddress(LPCSTR name)
{
    return wglGetProcAddress(name);
}

/* ---------------- layers: the main plane is all there is ---------------- */

BOOL WINAPI wglDescribeLayerPlane(HDC hdc, int format, int plane, UINT bytes, LPLAYERPLANEDESCRIPTOR descriptor)
{
    return FALSE;
}

int WINAPI wglSetLayerPaletteEntries(HDC hdc, int plane, int start, int entries, const COLORREF *colors)
{
    return 0;
}

int WINAPI wglGetLayerPaletteEntries(HDC hdc, int plane, int start, int entries, COLORREF *colors)
{
    return 0;
}

BOOL WINAPI wglRealizeLayerPalette(HDC hdc, int plane, BOOL realize)
{
    return FALSE;
}

/* ---------------- fonts ---------------- */

static BOOL xpwgl_use_font_bitmaps(HDC hdc, DWORD first, DWORD count, DWORD base, BOOL wide)
{
    static const MAT2 identity = { {0, 1}, {0, 0}, {0, 0}, {0, 1} };
    GLint old_alignment = 4;
    BOOL ok = TRUE;
    unsigned char *flipped = NULL;
    unsigned char *glyph = NULL;
    DWORD capacity = 0;

    glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    for (DWORD i = 0; i < count && ok; i++) {
        GLYPHMETRICS metrics;
        DWORD size;

        memset(&metrics, 0, sizeof(metrics));
        size = wide ? GetGlyphOutlineW(hdc, first + i, GGO_BITMAP, &metrics, 0, NULL, &identity)
                    : GetGlyphOutlineA(hdc, first + i, GGO_BITMAP, &metrics, 0, NULL, &identity);

        glNewList(base + i, GL_COMPILE);
        if (size != GDI_ERROR && size > 0) {
            if (size > capacity) {
                free(glyph);
                free(flipped);
                glyph = malloc(size);
                flipped = malloc(size);
                capacity = size;
            }
            if (glyph == NULL || flipped == NULL) {
                ok = FALSE;
            }
            else {
                DWORD got = wide ? GetGlyphOutlineW(hdc, first + i, GGO_BITMAP, &metrics, size, glyph, &identity)
                                 : GetGlyphOutlineA(hdc, first + i, GGO_BITMAP, &metrics, size, glyph, &identity);
                if (got != GDI_ERROR) {
                    /* GDI rows run top-down, GL bitmaps bottom-up */
                    DWORD stride = ((metrics.gmBlackBoxX + 31) / 32) * 4;
                    for (UINT y = 0; y < metrics.gmBlackBoxY; y++)
                        memcpy(flipped + y * stride, glyph + (metrics.gmBlackBoxY - 1 - y) * stride, stride);
                    glBitmap(metrics.gmBlackBoxX, metrics.gmBlackBoxY,
                             (GLfloat)-metrics.gmptGlyphOrigin.x,
                             (GLfloat)((int)metrics.gmBlackBoxY - metrics.gmptGlyphOrigin.y),
                             (GLfloat)metrics.gmCellIncX, (GLfloat)metrics.gmCellIncY, flipped);
                }
            }
        }
        else {
            glBitmap(0, 0, 0.0f, 0.0f, (GLfloat)metrics.gmCellIncX, (GLfloat)metrics.gmCellIncY, NULL);
        }
        glEndList();
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, old_alignment);
    free(glyph);
    free(flipped);
    return ok;
}

BOOL WINAPI wglUseFontBitmapsA(HDC hdc, DWORD first, DWORD count, DWORD base)
{
    return xpwgl_use_font_bitmaps(hdc, first, count, base, FALSE);
}

BOOL WINAPI wglUseFontBitmapsW(HDC hdc, DWORD first, DWORD count, DWORD base)
{
    return xpwgl_use_font_bitmaps(hdc, first, count, base, TRUE);
}

/* 3D outline fonts are not implemented; callers see a normal failure. */
BOOL WINAPI wglUseFontOutlinesA(HDC hdc, DWORD first, DWORD count, DWORD base, FLOAT deviation, FLOAT extrusion, int format, LPGLYPHMETRICSFLOAT metrics)
{
    return FALSE;
}

BOOL WINAPI wglUseFontOutlinesW(HDC hdc, DWORD first, DWORD count, DWORD base, FLOAT deviation, FLOAT extrusion, int format, LPGLYPHMETRICSFLOAT metrics)
{
    return FALSE;
}
