/*
 * OBS-style GL workload through EGL/Zink, as a regression test for the
 * event-sync NVK hang OBS Studio hit (mesa-nvk patch 0008), and for windowless
 * pbuffers (mesa-zink patch 0001).
 *
 * Mirrors what OBS's libobs-opengl does on Haiku:
 *  - EGL_DEFAULT_DISPLAY, eglBindAPI(EGL_OPENGL_API), a 3.3 core context,
 *    no window: made current with EGL_NO_SURFACE (or a pbuffer, --pbuffer);
 *  - thread A renders a textured quad into an FBO with an RGBA8 renderbuffer
 *    and reads it back with glReadPixels every frame (no eglSwapBuffers);
 *  - thread B takes the same context every few frames, uploads a new texture
 *    colour with glTexSubImage2D, glFlush()es and releases it;
 *  - the context is passed between the threads under a mutex, released with
 *    eglMakeCurrent(dpy, NO_SURFACE, NO_SURFACE, NO_CONTEXT).
 *
 * Every readback is checked: the quad must show the colour B uploaded last,
 * the rest the clear colour. A watchdog fails the test if no frame completes
 * for 10 s.
 *
 * Usage: egl-surfaceless-threads [--seconds N] [--pbuffer] [--fps N]
 * Exit status 0 = pass, 1 = stall or wrong pixels, 2 = setup failure.
 *
 * Build: cc -O2 -Wall -o egl-surfaceless-threads egl-surfaceless-threads.c -lEGL
 * (GL entry points come from eglGetProcAddress, so no GL headers or -lGL.)
 */
#include <EGL/egl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* EGL 1.5 / EGL_KHR_create_context names, in case the headers are older. */
#ifndef EGL_CONTEXT_MAJOR_VERSION
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#endif
#ifndef EGL_CONTEXT_MINOR_VERSION
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#endif
#ifndef EGL_CONTEXT_OPENGL_PROFILE_MASK
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#endif
#ifndef EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001
#endif

/* The few GL types, enums and entry points used, declared here so the test
 * needs no GL headers. */
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLboolean;
#ifndef APIENTRY
#define APIENTRY
#endif

#define GL_NO_ERROR             0
#define GL_TRIANGLES            0x0004
#define GL_TEXTURE_2D           0x0DE1
#define GL_UNSIGNED_BYTE        0x1401
#define GL_RGBA                 0x1908
#define GL_NEAREST              0x2600
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_COLOR_BUFFER_BIT     0x00004000
#define GL_RGBA8                0x8058
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_VERTEX_SHADER        0x8B31
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER          0x8D40
#define GL_READ_FRAMEBUFFER     0x8CA8
#define GL_RENDERBUFFER         0x8D41
#define GL_SCISSOR_TEST         0x0C11

#define GL_FUNCS(X) \
	X(GLenum, GetError, (void)) \
	X(void, Flush, (void)) \
	X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, Clear, (GLbitfield)) \
	X(void, Enable, (GLenum)) \
	X(void, Disable, (GLenum)) \
	X(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, GenFramebuffers, (GLsizei, GLuint *)) \
	X(void, BindFramebuffer, (GLenum, GLuint)) \
	X(void, GenRenderbuffers, (GLsizei, GLuint *)) \
	X(void, BindRenderbuffer, (GLenum, GLuint)) \
	X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
	X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
	X(GLenum, CheckFramebufferStatus, (GLenum)) \
	X(void, ReadBuffer, (GLenum)) \
	X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
	X(void, GenTextures, (GLsizei, GLuint *)) \
	X(void, BindTexture, (GLenum, GLuint)) \
	X(void, TexParameteri, (GLenum, GLenum, GLint)) \
	X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
	X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
	X(GLuint, CreateShader, (GLenum)) \
	X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
	X(void, CompileShader, (GLuint)) \
	X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
	X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(GLuint, CreateProgram, (void)) \
	X(void, AttachShader, (GLuint, GLuint)) \
	X(void, LinkProgram, (GLuint)) \
	X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
	X(void, UseProgram, (GLuint)) \
	X(void, GenVertexArrays, (GLsizei, GLuint *)) \
	X(void, BindVertexArray, (GLuint)) \
	X(void, DrawArrays, (GLenum, GLint, GLsizei))

#define DECLARE(ret, name, args) static ret (APIENTRY *gl##name) args;
GL_FUNCS(DECLARE)
#undef DECLARE

enum { kSize = 256, kTexSize = 64 };

static EGLDisplay gDpy;
static EGLContext gCtx;
static EGLSurface gSurf = EGL_NO_SURFACE;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool gStop;
static volatile bool gFailed;
static volatile uint64_t gFrames;
static volatile uint64_t gUploads;
static uint32_t gTexColor;   /* RGBA bytes B uploaded last; guarded by gLock */
static GLuint gFbo, gTex, gProg, gVao;
static int gFps;

static void
fail(const char *msg)
{
	fprintf(stderr, "FAIL: %s\n", msg);
	gFailed = true;
	gStop = true;
}

static void
take_context(void)
{
	pthread_mutex_lock(&gLock);
	if (!eglMakeCurrent(gDpy, gSurf, gSurf, gCtx)) {
		fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
		exit(2);
	}
}

static void
release_context(void)
{
	/* What OBS does on release. */
	eglMakeCurrent(gDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	pthread_mutex_unlock(&gLock);
}

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
upload_color(uint32_t rgba)
{
	static uint32_t pixels[kTexSize * kTexSize];
	for (int i = 0; i < kTexSize * kTexSize; i++)
		pixels[i] = rgba;
	glBindTexture(GL_TEXTURE_2D, gTex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTexSize, kTexSize, GL_RGBA,
	                GL_UNSIGNED_BYTE, pixels);
	gTexColor = rgba;
}

static GLuint
compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader compile failed: %s\n", log);
		exit(2);
	}
	return s;
}

static void
setup_gl(void)
{
	/* The "swapchain": an FBO with an RGBA8 renderbuffer. */
	GLuint rb;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kSize, kSize);
	glGenFramebuffers(1, &gFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, gFbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                          GL_RENDERBUFFER, rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "framebuffer incomplete\n");
		exit(2);
	}

	glGenTextures(1, &gTex);
	glBindTexture(GL_TEXTURE_2D, gTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTexSize, kTexSize, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	upload_color(0xff0000ffu);   /* opaque red (bytes R,G,B,A = ff,00,00,ff) */

	/* A quad over the left half, sampling the texture. */
	static const char *vs =
		"#version 330 core\n"
		"out vec2 uv;\n"
		"void main() {\n"
		"  vec2 p[6] = vec2[](vec2(-1,-1), vec2(0,-1), vec2(0,1),\n"
		"                     vec2(-1,-1), vec2(0,1), vec2(-1,1));\n"
		"  uv = (p[gl_VertexID] + 1.0) * 0.5;\n"
		"  gl_Position = vec4(p[gl_VertexID], 0.0, 1.0);\n"
		"}\n";
	static const char *fs =
		"#version 330 core\n"
		"uniform sampler2D tex;\n"
		"in vec2 uv;\n"
		"out vec4 color;\n"
		"void main() { color = texture(tex, uv); }\n";
	gProg = glCreateProgram();
	glAttachShader(gProg, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(gProg, compile(GL_FRAGMENT_SHADER, fs));
	glLinkProgram(gProg);
	GLint ok = 0;
	glGetProgramiv(gProg, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "program link failed\n");
		exit(2);
	}
	glGenVertexArrays(1, &gVao);
}

/* Thread A: render + synchronous readback, like OBS's graphics thread. */
static void *
render_thread(void *arg)
{
	(void)arg;
	static uint32_t pixels[kSize * kSize];
	const uint32_t clear = 0xff102030u;   /* bytes 30,20,10,ff in memory */
	while (!gStop) {
		take_context();
		glBindFramebuffer(GL_FRAMEBUFFER, gFbo);
		glViewport(0, 0, kSize, kSize);
		glClearColor(0x30 / 255.0f, 0x20 / 255.0f, 0x10 / 255.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(gProg);
		glBindVertexArray(gVao);
		glBindTexture(GL_TEXTURE_2D, gTex);
		glDrawArrays(GL_TRIANGLES, 0, 6);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, gFbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		GLenum err = glGetError();
		uint32_t expectQuad = gTexColor;
		release_context();

		if (err != GL_NO_ERROR) {
			fprintf(stderr, "GL error 0x%x\n", err);
			fail("GL error during render/readback");
			break;
		}
		/* Middle of the left half: the quad; middle of the right half: clear. */
		uint32_t quad = pixels[(kSize / 2) * kSize + kSize / 4];
		uint32_t back = pixels[(kSize / 2) * kSize + 3 * kSize / 4];
		if (quad != expectQuad || back != clear) {
			fprintf(stderr, "frame %llu: quad 0x%08x (want 0x%08x), "
			        "background 0x%08x (want 0x%08x)\n",
			        (unsigned long long)gFrames, quad, expectQuad, back, clear);
			fail("wrong pixels read back");
			break;
		}
		gFrames++;
		if (gFps > 0)
			usleep(1000000 / gFps);
	}
	return NULL;
}

/* Thread B: borrow the context for an upload, like OBS's UI thread. */
static void *
upload_thread(void *arg)
{
	(void)arg;
	uint32_t n = 0;
	while (!gStop) {
		usleep(15000);
		take_context();
		n++;
		uint32_t rgba = 0xff000000u | ((n * 0x3b) & 0xff) << 16 |
		                ((n * 0x71) & 0xff) << 8 | ((n * 0x17) & 0xff);
		upload_color(rgba);
		glFlush();
		release_context();
		gUploads++;
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	double seconds = 20;
	bool usePbuffer = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			seconds = atof(argv[++i]);
		else if (strcmp(argv[i], "--pbuffer") == 0)
			usePbuffer = true;
		else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
			gFps = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: %s [--seconds N] [--pbuffer] [--fps N]\n", argv[0]);
			return 2;
		}
	}

	gDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (gDpy == EGL_NO_DISPLAY || !eglInitialize(gDpy, NULL, NULL)) {
		fprintf(stderr, "setup failed: eglInitialize\n");
		return 2;
	}
	if (!eglBindAPI(EGL_OPENGL_API)) {
		fprintf(stderr, "setup failed: eglBindAPI\n");
		return 2;
	}
	const EGLint cfgAttribs[] = {
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_SURFACE_TYPE, usePbuffer ? EGL_PBUFFER_BIT : 0,
		EGL_NONE,
	};
	EGLConfig cfg;
	EGLint numCfg = 0;
	if (!eglChooseConfig(gDpy, cfgAttribs, &cfg, 1, &numCfg) || numCfg < 1) {
		fprintf(stderr, "setup failed: no EGL config\n");
		return 2;
	}
	const EGLint ctxAttribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE,
	};
	gCtx = eglCreateContext(gDpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
	if (gCtx == EGL_NO_CONTEXT) {
		fprintf(stderr, "setup failed: eglCreateContext 0x%x\n", eglGetError());
		return 2;
	}
	if (usePbuffer) {
		const EGLint pbAttribs[] = { EGL_WIDTH, kSize, EGL_HEIGHT, kSize, EGL_NONE };
		gSurf = eglCreatePbufferSurface(gDpy, cfg, pbAttribs);
		if (gSurf == EGL_NO_SURFACE) {
			fprintf(stderr, "setup failed: eglCreatePbufferSurface 0x%x\n", eglGetError());
			return 2;
		}
	}

	take_context();
#define LOAD(ret, name, args) \
	gl##name = (ret (APIENTRY *) args)eglGetProcAddress("gl" #name); \
	if (gl##name == NULL) { fprintf(stderr, "setup failed: no gl" #name "\n"); return 2; }
	GL_FUNCS(LOAD)
#undef LOAD
	setup_gl();
	if (usePbuffer) {
		/* eglSwapBuffers on a pbuffer must be a harmless no-op. */
		if (!eglSwapBuffers(gDpy, gSurf)) {
			fprintf(stderr, "eglSwapBuffers on a pbuffer failed: 0x%x\n", eglGetError());
			return 1;
		}
	}
	release_context();
	printf("%s context, OpenGL 3.3 core; running %.0f s\n",
	       usePbuffer ? "pbuffer" : "surfaceless", seconds);

	pthread_t a, b;
	pthread_create(&a, NULL, render_thread, NULL);
	pthread_create(&b, NULL, upload_thread, NULL);

	double start = now_s(), lastProgress = start;
	uint64_t lastFrames = 0;
	while (!gStop) {
		usleep(200000);
		double t = now_s();
		uint64_t f = gFrames;
		if (f != lastFrames) {
			lastFrames = f;
			lastProgress = t;
		} else if (t - lastProgress > 10.0) {
			printf("FAIL: no frame completed for 10 s (stalled after %llu frames, %llu uploads)\n",
			       (unsigned long long)f, (unsigned long long)gUploads);
			fflush(stdout);
			_exit(1);   /* the render thread is stuck; don't join it */
		}
		if (t - start >= seconds)
			gStop = true;
	}
	pthread_join(a, NULL);
	pthread_join(b, NULL);

	double elapsed = now_s() - start;
	if (gFailed)
		return 1;
	printf("PASS: %llu frames (%.0f fps), %llu context hand-offs for uploads\n",
	       (unsigned long long)gFrames, gFrames / elapsed,
	       (unsigned long long)gUploads);

	if (gSurf != EGL_NO_SURFACE)
		eglDestroySurface(gDpy, gSurf);
	eglDestroyContext(gDpy, gCtx);
	eglTerminate(gDpy);
	return 0;
}
