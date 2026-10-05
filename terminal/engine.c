// Android lifecycle/PTY adapter. Screen, parser, shaping and glyph rasterization
// are compiled from upstream kitty; this file does not interpret escape codes.
#include "fonts.h"
#include "keys.h"
#include "engine.h"
#include <jni.h>
#include <android/log.h>
#include <pthread.h>

extern PyObject* PyInit_fast_data_types(void);
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool initialized;
static bool attempted;
static PyObject *engine;
static Screen *screen;
static FONTS_DATA_HANDLE fonts;
static float font_size = 12.f, requested_font_size = 12.f;
static char error_text[2048];
typedef struct KittySession {
    int id; Screen* screen; char* replies; size_t reply_size, reply_capacity;
    struct KittySession* next;
} KittySession;
static KittySession *sessions, *working;
static int active_id;
static bool select_session(int id) {
    KittySession* value = sessions;
    while (value && value->id != id) value = value->next;
    PyObject* selected = PyObject_CallMethod(engine,"select","i",id);
    if (!selected) return false;
    if (!value) {
        value = calloc(1,sizeof(*value));
        if (!value) { Py_DECREF(selected); PyErr_NoMemory(); return false; }
        value->id = id; value->screen = (Screen*)selected; value->next = sessions; sessions = value;
    } else Py_DECREF(selected);
    working = value; screen = value->screen; return true;
}

static void python_error(void) {
    PyObject *type=NULL, *value=NULL, *traceback=NULL;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    PyObject *text=value ? PyObject_Str(value) : NULL;
    snprintf(error_text, sizeof(error_text), "%s", text ? PyUnicode_AsUTF8(text) : "kitty Python initialization failed");
    Py_XDECREF(text);
    PyErr_Restore(type, value, traceback); PyErr_Print();
    __android_log_print(ANDROID_LOG_ERROR, "goblin-kitty", "%s", error_text);
}
static PyObject* host_write(PyObject* self, PyObject* arg) {
    char *data; Py_ssize_t size;
    if (PyBytes_AsStringAndSize(arg, &data, &size) < 0) return NULL;
    if ((size_t)size > SIZE_MAX-working->reply_size) return PyErr_NoMemory();
    size_t needed=working->reply_size+(size_t)size;
    if (needed>working->reply_capacity) {
        size_t capacity=needed>SIZE_MAX/2 ? needed : MAX(needed,working->reply_capacity*2);
        char* grown=realloc(working->replies,capacity);
        if (!grown) return PyErr_NoMemory();
        working->replies=grown; working->reply_capacity=capacity;
    }
    memcpy(working->replies+working->reply_size, data, size); working->reply_size+=size;
    Py_RETURN_NONE;
}
static PyObject* host_sprite(PyObject* self, PyObject* args) {
    unsigned x,y,z; const char* data; Py_ssize_t size;
    if (!PyArg_ParseTuple(args, "IIIy#", &x,&y,&z,&data,&size)) return NULL;
    GoblinRenderSprite(x,y,z,data,size); Py_RETURN_NONE;
}
static PyMethodDef host_methods[] = {
    {"write",host_write,METH_O,NULL}, {"sprite",host_sprite,METH_VARARGS,NULL}, {NULL}
};
static PyModuleDef host_module = {PyModuleDef_HEAD_INIT, "_goblin_android", NULL, -1, host_methods};
static PyObject* init_host(void) { return PyModule_Create(&host_module); }

JNIEXPORT jstring JNICALL Java_dev_goblinlinux_sentry_KittyRuntime_initializeNative(JNIEnv* env, jclass cls, jstring directory) {
    pthread_mutex_lock(&mutex);
    if (attempted) { pthread_mutex_unlock(&mutex); return (*env)->NewStringUTF(env, error_text); }
    attempted=true;
    const char *dir=(*env)->GetStringUTFChars(env,directory,NULL);
    char path[4096];
    snprintf(path,sizeof(path),"%s/fonts.conf",dir); setenv("FONTCONFIG_FILE",path,1);
    snprintf(path,sizeof(path),"%s/cache",dir); setenv("KITTY_CACHE_DIRECTORY",path,1); setenv("TMPDIR",path,1);
    PyImport_AppendInittab("kitty.fast_data_types",PyInit_fast_data_types);
    PyImport_AppendInittab("_goblin_android",init_host);
    PyConfig config; PyConfig_InitIsolatedConfig(&config);
    config.site_import=0; config.install_signal_handlers=0; config.write_bytecode=0;
    config.module_search_paths_set=1;
    const char* suffixes[]={"/python313.zip","/lib-dynload","/app"};
    for (unsigned i=0;i<arraysz(suffixes);i++) {
        snprintf(path,sizeof(path),"%s%s",dir,suffixes[i]);
        wchar_t* wide=Py_DecodeLocale(path,NULL); PyWideStringList_Append(&config.module_search_paths,wide); PyMem_RawFree(wide);
    }
    PyConfig_SetBytesString(&config,&config.home,dir);
    PyConfig_SetBytesString(&config,&config.program_name,"goblin-kitty");
    PyStatus status=Py_InitializeFromConfig(&config); PyConfig_Clear(&config);
    (*env)->ReleaseStringUTFChars(env,directory,dir);
    if (PyStatus_Exception(status)) {
        snprintf(error_text,sizeof(error_text),"%s",status.err_msg ? status.err_msg : "Python initialization failed");
    } else {
        engine=PyImport_ImportModule("engine");
        if (engine) {
            select_session(0);
            fonts=load_fonts_data(12,160,160);
            initialized=screen && fonts;
        }
        if (!initialized) python_error();
        PyEval_SaveThread();
    }
    pthread_mutex_unlock(&mutex);
    return (*env)->NewStringUTF(env,error_text);
}

void GoblinKittyFeedSession(int id,const char* bytes,size_t size) {
    pthread_mutex_lock(&mutex);
    if (initialized) {
        PyGILState_STATE gil=PyGILState_Ensure();
        if (!select_session(id)) size=0;
        while (size) {
            size_t capacity=0; uint8_t* dest=vt_parser_create_write_buffer(screen->vt_parser,&capacity);
            size_t n=MIN(size,capacity);
            if (n) { memcpy(dest,bytes,n); vt_parser_commit_write(screen->vt_parser,n); bytes+=n; size-=n; }
            ParseData parsed={.now=monotonic()}; parse_worker(screen,&parsed,true);
            if (PyErr_Occurred()) { python_error(); break; }
            if (!n) break;
        }
        PyGILState_Release(gil);
    }
    pthread_mutex_unlock(&mutex);
}
void GoblinKittyReset(void) {
    pthread_mutex_lock(&mutex);
    if (initialized) {
        PyGILState_STATE gil=PyGILState_Ensure();
        select_session(active_id);
        PyObject* r=PyObject_CallMethod((PyObject*)screen,"reset",NULL); Py_XDECREF(r);
        if (PyErr_Occurred()) python_error();
        working->reply_size=0; PyGILState_Release(gil);
    }
    pthread_mutex_unlock(&mutex);
}
void GoblinKittyFeed(const char* bytes,size_t size) { GoblinKittyFeedSession(0,bytes,size); }
size_t GoblinKittyTakeSessionInput(int id,char* bytes,size_t capacity) {
    pthread_mutex_lock(&mutex);
    KittySession* value = sessions; while (value && value->id != id) value = value->next;
    size_t n=value ? MIN(capacity,value->reply_size) : 0;
    if (value) { memcpy(bytes,value->replies,n); memmove(value->replies,value->replies+n,value->reply_size-n); value->reply_size-=n; }
    pthread_mutex_unlock(&mutex); return n;
}
size_t GoblinKittyTakeInput(char* bytes,size_t capacity) { return GoblinKittyTakeSessionInput(0,bytes,capacity); }
void GoblinKittySelect(int id) {
    pthread_mutex_lock(&mutex); active_id=id;
    if (initialized) { PyGILState_STATE gil=PyGILState_Ensure(); if (!select_session(id)) python_error(); PyGILState_Release(gil); }
    pthread_mutex_unlock(&mutex);
}
void GoblinKittyClose(int id) {
    pthread_mutex_lock(&mutex);
    if (initialized && id != 0) {
        PyGILState_STATE gil=PyGILState_Ensure();
        if (active_id == id) active_id=0;
        select_session(active_id);
        KittySession** at=&sessions;
        while (*at && (*at)->id != id) at=&(*at)->next;
        if (*at) { KittySession* old=*at; *at=old->next; Py_DECREF(old->screen); free(old->replies); free(old); }
        PyObject* r=PyObject_CallMethod(engine,"close","i",id); Py_XDECREF(r);
        if (PyErr_Occurred()) python_error(); PyGILState_Release(gil);
    }
    pthread_mutex_unlock(&mutex);
}
JNIEXPORT void JNICALL Java_dev_goblinlinux_sentry_KittyRuntime_contextNative(JNIEnv* env,jclass cls) {
    pthread_mutex_lock(&mutex); GoblinRenderReset(); pthread_mutex_unlock(&mutex);
}
JNIEXPORT jintArray JNICALL Java_dev_goblinlinux_sentry_KittyRuntime_drawNative(JNIEnv* env,jclass cls,jint width,jint height) {
    jint dims[2]={24,80};
    pthread_mutex_lock(&mutex);
    if (initialized && width>0 && height>0) {
        PyGILState_STATE gil=PyGILState_Ensure();
        if (font_size != requested_font_size) {
            GoblinRenderClearSprites();
            PyObject* resized = PyObject_CallMethod(engine,"set_font_size","d",(double)requested_font_size);
            if (!resized) python_error();
            Py_XDECREF(resized);
            fonts = load_fonts_data(requested_font_size,160,160);
            font_size = requested_font_size;
            for (KittySession* value=sessions; value; value=value->next) screen_dirty_sprite_positions(value->screen);
        }
        select_session(active_id);
        unsigned cw=fonts->fcm.cell_width,ch=fonts->fcm.cell_height;
        screen->cell_size.width=cw; screen->cell_size.height=ch;
        unsigned rows=MIN(250u,MAX(1u,(unsigned)height/ch)),cols=MIN(500u,MAX(1u,(unsigned)width/cw));
        dims[0]=rows; dims[1]=cols;
        if (rows!=screen->lines || cols!=screen->columns) {
            PyObject* r=PyObject_CallMethod((PyObject*)screen,"resize","II",rows,cols); Py_XDECREF(r);
        }
        GoblinCell* cells=calloc(rows*cols,sizeof(*cells));
        if (cells) {
            for (unsigned y=0;y<rows;y++) {
                Line* line=(Line*)PyObject_CallMethod((PyObject*)screen,"visual_line","I",y);
                if (!line) break;
                RAII_ListOfChars(chars);
                render_line(fonts,line,y,NULL,DISABLE_LIGATURES_NEVER,&chars);
                for (unsigned x=0;x<cols;x++) {
                    unsigned idx=x; color_type fg=0xffffff,bg=0x000000; bool reversed=false;
                    colors_for_cell(line,screen->color_profile,&idx,&fg,&bg,&reversed);
                    if (screen->modes.mDECSCNM) { color_type t=fg;fg=bg;bg=t; }
                    const GPUCell* g=line->gpu_cells+x;
                    cells[y*cols+x]=(GoblinCell){g->sprite_idx,fg,bg,fg,g->attrs.val};
                }
                Py_DECREF(line);
            }
            unsigned ax,ay,az; sprite_tracker_current_layout(fonts,&ax,&ay,&az);
            bool cursor=screen->modes.mDECTCEM && !screen->scrolled_by;
            GoblinRenderFrame(cells,rows,cols,cw,ch,ax,ay,cursor?(int)screen->cursor->x:-1,cursor?(int)screen->cursor->y:-1,width,height);
            free(cells);
        }
        if (PyErr_Occurred()) python_error();
        PyGILState_Release(gil);
    }
    pthread_mutex_unlock(&mutex);
    jintArray result=(*env)->NewIntArray(env,2); (*env)->SetIntArrayRegion(env,result,0,2,dims); return result;
}
JNIEXPORT void JNICALL Java_dev_goblinlinux_sentry_KittyRuntime_fontSizeNative(JNIEnv* env,jclass cls,jfloat points) {
    if (!(points >= 6.f && points <= 36.f)) return;
    pthread_mutex_lock(&mutex); requested_font_size=points; pthread_mutex_unlock(&mutex);
}
JNIEXPORT jbyteArray JNICALL Java_dev_goblinlinux_sentry_KittyRuntime_dumpNative(JNIEnv* env,jclass cls) {
    pthread_mutex_lock(&mutex); jbyteArray out=(*env)->NewByteArray(env,0);
    if (initialized) {
        PyGILState_STATE gil=PyGILState_Ensure(); select_session(active_id); PyObject* text=PyObject_CallMethod(engine,"dump",NULL);
        if (text && PyBytes_Check(text)) {
            out=(*env)->NewByteArray(env,PyBytes_GET_SIZE(text));
            (*env)->SetByteArrayRegion(env,out,0,PyBytes_GET_SIZE(text),(const jbyte*)PyBytes_AS_STRING(text));
        }
        Py_XDECREF(text); if (PyErr_Occurred()) python_error(); PyGILState_Release(gil);
    }
    pthread_mutex_unlock(&mutex); return out;
}
JNIEXPORT void JNICALL Java_dev_goblinlinux_sentry_KittyRuntime_keyNative(JNIEnv* env,jclass cls,jint key,jint mods,jstring text) {
    pthread_mutex_lock(&mutex);
    if (initialized) {
        PyGILState_STATE gil=PyGILState_Ensure();
        select_session(active_id);
        const jchar* utf16=(*env)->GetStringChars(env,text,NULL);
        PyObject* value=PyUnicode_DecodeUTF16((const char*)utf16,2*(*env)->GetStringLength(env,text),"strict",NULL);
        (*env)->ReleaseStringChars(env,text,utf16);
        PyObject* r=value ? PyObject_CallMethod(engine,"key","iiO",key,mods,value) : NULL;
        Py_XDECREF(r); Py_XDECREF(value); if (PyErr_Occurred()) python_error(); PyGILState_Release(gil);
    }
    pthread_mutex_unlock(&mutex);
}

JNIEXPORT void JNICALL Java_dev_goblinlinux_sentry_KittyRuntime_scrollNative(JNIEnv* env,jclass cls,jint lines) {
    pthread_mutex_lock(&mutex);
    if (initialized) {
        PyGILState_STATE gil=PyGILState_Ensure();
        select_session(active_id);
        PyObject* r=PyObject_CallMethod(engine,"scroll","i",lines); Py_XDECREF(r);
        if (PyErr_Occurred()) python_error(); PyGILState_Release(gil);
    }
    pthread_mutex_unlock(&mutex);
}
