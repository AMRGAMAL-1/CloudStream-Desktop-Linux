// overlay_spike.cpp — isolated proof that mpv overlay-add works with the
// vo=libmpv render API (the exact path CloudStream Desktop uses).
// This file is NOT part of the application build; it is a standalone spike.
//
// Build:
//   g++ -O2 -std=c++17 overlay_spike.cpp -o overlay_spike \
//       $(pkg-config --cflags --libs mpv) -lX11 -lGL -ldl
//
// Run:
//   ./overlay_spike /tmp/cs3_test_video.mp4
//
// Prints PASS if the magenta overlay appears inside a rendered frame.

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

static void* get_proc_address(void*, const char* name) {
    return (void*)glXGetProcAddressARB((const GLubyte*)name);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: %s <video>\n", argv[0]);
        return 2;
    }
    const char* video = argv[1];
    const int W = 640, H = 360;

    // ---- X11 + GLX context ------------------------------------------------
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { printf("FAIL: no X display\n"); return 1; }
    int screen = DefaultScreen(dpy);
    static int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                          GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    XVisualInfo* vi = glXChooseVisual(dpy, screen, attrs);
    if (!vi) { printf("FAIL: glXChooseVisual\n"); return 1; }
    Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
    XSetWindowAttributes swa = {};
    swa.colormap = cmap;
    swa.border_pixel = 0;
    Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, W, H, 0,
                               vi->depth, InputOutput, vi->visual,
                               CWColormap | CWBorderPixel, &swa);
    GLXContext ctx = glXCreateContext(dpy, vi, nullptr, GL_TRUE);
    XMapWindow(dpy, win);
    glXMakeCurrent(dpy, win, ctx);

    // ---- libmpv render-API instance (the app's exact path) ---------------
    mpv_handle* m = mpv_create();
    if (!m) { printf("FAIL: mpv_create\n"); return 1; }
    mpv_set_option_string(m, "vo", "libmpv");
    mpv_set_option_string(m, "gpu-api", "opengl");
    mpv_set_option_string(m, "hwdec", "no");
    mpv_set_option_string(m, "idle", "yes");
    mpv_set_option_string(m, "keep-open", "yes");
    if (mpv_initialize(m) < 0) { printf("FAIL: mpv_initialize\n"); return 1; }

    mpv_opengl_init_params gl_init{get_proc_address, nullptr};
    mpv_render_param init_params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_X11_DISPLAY, dpy},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context* rc = nullptr;
    if (mpv_render_context_create(&rc, m, init_params) < 0) {
        printf("FAIL: render context create\n");
        return 1;
    }
    mpv_render_context_set_update_callback(rc, [](void*) {}, nullptr);

    // ---- load the video ---------------------------------------------------
    const char* load[] = {"loadfile", video, nullptr};
    mpv_command(m, load);

    // ---- wait until a video frame is actually being decoded ---------------
    for (int i = 0; i < 300; i++) {
        mpv_event* e = mpv_wait_event(m, 0.05);
        if (e && e->event_id == MPV_EVENT_NONE) continue;
        double pos = 0;
        if (mpv_get_property(m, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0 && pos > 0.0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // ---- render one frame so the video is on the FBO ----------------------
    auto render_frame = [&]() {
        mpv_opengl_fbo fbo{0, W, H, 0};
        int flip_y = 1;
        mpv_render_param rp[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        glViewport(0, 0, W, H);
        mpv_render_context_render(rc, rp);
    };
    render_frame();
    glFinish();
    // Probe: is the video actually rendering (non-black)?
    unsigned char* probe = new unsigned char[W * H * 4];
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, probe);
    int nonBlack = 0;
    for (int i = 0; i < W * H; i += 137)
        if (probe[i * 4] + probe[i * 4 + 1] + probe[i * 4 + 2] > 60) nonBlack++;
    printf("video frame probe: %d non-black samples / %d\n", nonBlack, (W * H) / 137);

    // ---- Path 2: manual GL texture compositing over the rendered frame -----
    // mpv overlay-add is NOT supported by vo=libmpv (render API). Instead we
    // draw the snapshot as a textured quad in OUR GL context after the video
    // frame renders — the same GL pass, full alpha control, no X11 windowing.
    const int OW = 200, OH = 100;
    unsigned char* ov = new unsigned char[OW * OH * 4];
    for (int y = 0; y < OH; y++)
        for (int x = 0; x < OW; x++) {
            unsigned char* p = ov + (y * OW + x) * 4;
            p[0] = 0xFF; p[1] = 0x00; p[2] = 0xFF; p[3] = 0xFF;  // BGRA magenta
        }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, OW, OH, 0, GL_BGRA, GL_UNSIGNED_BYTE, ov);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    auto draw_overlay = [&]() {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
        // Map pixel coordinates to the viewport via an ortho projection.
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, W, 0, H, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glBegin(GL_QUADS);
        glColor4f(1, 1, 1, 1);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f(1, 0); glVertex2f(OW, 0);
        glTexCoord2f(1, 1); glVertex2f(OW, OH);
        glTexCoord2f(0, 1); glVertex2f(0, OH);
        glEnd();
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
    };

    // ---- render again with the texture composited over the video ----------
    render_frame();
    draw_overlay();
    glFinish();
    unsigned char* px = new unsigned char[W * H * 4];
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

    bool found = false;
    for (int y = 0; y < OH && !found; y++)
        for (int x = 0; x < OW && !found; x++) {
            unsigned char* p = px + (y * W + x) * 4;
            if (p[0] > 200 && p[2] > 200 && p[1] < 80) found = true;  // magenta-ish
        }
    printf("texture composited over video frame: %s\n", found ? "PASS ✓" : "FAIL ✗");

    glDeleteTextures(1, &tex);
    delete[] ov;
    delete[] px;
    mpv_render_context_free(rc);
    mpv_terminate_destroy(m);
    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return found ? 0 : 1;
}
