/*
 * qembian on Android (android/README.md): the JNI side of libqemu-syborg.so, compiled into QEMU only by
 * android/native/build.sh. QEMU runs on its own thread in the app's process; the Kotlin activity
 * (android/app, class org.qembian.phone.Native) starts it with the bundle's arguments, hands over the
 * window, and forwards touches and keys. Everything that touches QEMU state runs on QEMU's main loop
 * (a bottom half), never on the Java threads.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu-main.h"
#include "qemu/aio.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/replay.h"
#include "ui/input.h"
#include "qom/object.h"
#include "qapi/util.h"
#include "qapi/qapi-types-ui.h"
#include "syborg.h"
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <sys/resource.h>

#define TAG "qembian"

/* bionic has no POSIX shared memory; QEMU's memory backends that ask for it fail cleanly */
int shm_open(const char *name, int flags, mode_t mode)
{
    errno = ENOSYS;
    return -1;
}

int shm_unlink(const char *name)
{
    errno = ENOSYS;
    return -1;
}

/* ---- QEMU's stderr and stdout into logcat (one line per message) */
static void *log_pump(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
        if (n <= 0) {
            return NULL;
        }
        len += n;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', buf + len - start))) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, TAG, start);
            start = nl + 1;
        }
        len -= start - buf;
        memmove(buf, start, len);
        if (len == sizeof(buf) - 1) {
            buf[len] = 0;
            __android_log_write(ANDROID_LOG_INFO, TAG, buf);
            len = 0;
        }
    }
}

static void log_redirect(void)
{
    int p[2];
    pthread_t t;
    if (pipe(p) == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[1]);
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0]);
        pthread_detach(t);
    }
}

/* ---- the emulator thread: what QEMU's main() does (system/main.c, not linked into the library) */
__attribute__((weak)) int (*qemu_main)(void);   /* main.c's in the executable; ours in the library */
static int q_argc;
static char **q_argv;

static void *qemu_thread_main(void *unused)
{
    /* QEMU's threads (vCPU, GL, main loop) inherit this: above Android's normal app threads, so the
       scheduler keeps the vCPU on a big core and does not preempt it for background work */
    setpriority(PRIO_PROCESS, 0, -10);
    qemu_init(q_argc, q_argv);
    bql_unlock();
    replay_mutex_unlock();
    replay_mutex_lock();                /* main.c's qemu_default_main */
    bql_lock();
    int status = qemu_main_loop();
    qemu_cleanup(status);
    bql_unlock();
    replay_mutex_unlock();
    exit(status);                       /* the app's process goes with it (Exit) */
    return NULL;
}

JNIEXPORT void JNICALL Java_org_qembian_phone_Native_start(JNIEnv *env, jclass cls, jobjectArray args)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    log_redirect();
    q_argc = (*env)->GetArrayLength(env, args) + 1;
    q_argv = g_new0(char *, q_argc + 1);
    q_argv[0] = g_strdup("qemu-system-arm");
    for (int i = 1; i < q_argc; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, args, i - 1);
        const char *c = (*env)->GetStringUTFChars(env, s, NULL);
        q_argv[i] = g_strdup(c);
        (*env)->ReleaseStringUTFChars(env, s, c);
        (*env)->DeleteLocalRef(env, s);
    }
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20);    /* QEMU's main thread expects a desktop-sized stack */
    pthread_create(&t, &a, qemu_thread_main, NULL);
    pthread_detach(t);
}

/* ---- input: queued as bottom halves on QEMU's main loop */
typedef struct { int kind, id, a, b, c; } InEv;
enum { EV_TOUCH, EV_KEY, EV_QUIT };

static void in_bh(void *opaque)
{
    InEv *e = opaque;
    switch (e->kind) {
    case EV_TOUCH:                      /* a = x, b = y (0..0x7fff of the picture), c = down */
        qemu_input_queue_abs(NULL, INPUT_AXIS_X, e->a, 0, 0x7fff);
        qemu_input_queue_abs(NULL, INPUT_AXIS_Y, e->b, 0, 0x7fff);
        qemu_input_queue_btn(NULL, INPUT_BUTTON_LEFT, e->c);
        qemu_input_event_sync();
        break;
    case EV_KEY:                        /* a = QKeyCode, b = down: as a Linux key code (QEMU 11) */
        if ((guint)e->a < qemu_input_map_qcode_to_linux_len) {
            qemu_input_event_send_key_linux(NULL, qemu_input_map_qcode_to_linux[e->a], e->b);
        }
        break;
    case EV_QUIT:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
        break;
    }
    g_free(e);
}

static void in_post(int kind, int id, int a, int b, int c)
{
    InEv *e = g_new(InEv, 1);
    *e = (InEv){ kind, id, a, b, c };
    aio_bh_schedule_oneshot(qemu_get_aio_context(), in_bh, e);
}

JNIEXPORT void JNICALL Java_org_qembian_phone_Native_touch(JNIEnv *env, jclass cls, jint id, jint x, jint y,
                                                           jboolean down)
{
    in_post(EV_TOUCH, id, x, y, down);
}

/* qcode: a QKeyCode name ("f8", "volumeup"), as the bundle's key list gives them */
JNIEXPORT void JNICALL Java_org_qembian_phone_Native_key(JNIEnv *env, jclass cls, jstring qcode, jboolean down)
{
    const char *c = (*env)->GetStringUTFChars(env, qcode, NULL);
    int q = qapi_enum_parse(&QKeyCode_lookup, c, -1, NULL);
    (*env)->ReleaseStringUTFChars(env, qcode, c);
    if (q >= 0) {
        in_post(EV_KEY, 0, q, down, 0);
    }
}

JNIEXPORT void JNICALL Java_org_qembian_phone_Native_quit(JNIEnv *env, jclass cls)
{
    in_post(EV_QUIT, 0, 0, 0, 0);
}

/* ---- the window: syborg-gl presents into it (present=window) */
JNIEXPORT void JNICALL Java_org_qembian_phone_Native_setWindow(JNIEnv *env, jclass cls, jobject surface)
{
    ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    syborg_gl_set_window(w);            /* takes the reference (NULL: the window is gone) */
}
