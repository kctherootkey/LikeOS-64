// gltile -- paint tiles on the GPU the way WebKit's Skia backend does, and
// check every pixel.
//
// The web process paints each tile into a pooled RGBA8 texture that Skia
// wraps as a render target: draws go into an 8x multisampled colour buffer
// with a multisampled stencil attachment, are resolved into the texture, and
// the compositor then samples that texture from another draw.  This program
// repeats that cycle with a scene it can also rasterise on the processor --
// axis-aligned rectangles on whole-pixel bounds, solid or with a horizontal
// gradient, some behind a scissor, some behind a stencil clip -- and compares
// the tile and the composited copy against the reference after every paint.
// Every mismatch is reported with where it is and whether the wrong pixels
// are what the tile held the LAST time it was painted (stale content) or
// something else.
//
// EGL on the GBM platform with a surfaceless GLES 3 context, as the web
// process runs (-S uses the surfaceless platform instead, and a build with
// -DNO_GBM has only that).  Options:
//   -n N    paints (default 300)
//   -s S    samples for the multisampled buffers, 0 = paint directly (8)
//   -t T    tile size (512)
//   -p P    textures in the pool (8)
//   -2      sample the tile from a second, shared context (the compositor)
//   -d DEV  render node (/dev/dri/renderD128)
//   -S      EGL surfaceless platform instead of GBM
//   -v      print every paint
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef NO_GBM
#include <gbm.h>
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

static int tile = 512, pool_n = 8, iters = 300, samples = 8, verbose = 0,
	   two_ctx = 0, surfaceless = 0;
static const char *devpath = "/dev/dri/renderD128";

/* ---- a tiny deterministic scene -------------------------------------- */

static uint32_t rng_state = 0x1234567u;
static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

struct op {
	int kind; /* 0 solid, 1 gradient, 2 scissor+solid, 3 stencil-clipped solid */
	int x, y, w, h;   /* the rectangle drawn */
	int cx, cy, cw, ch; /* the clip (scissor or stencil) for kinds 2, 3 */
	uint8_t c0[4], c1[4];
};

#define MAX_OPS 24
struct scene {
	uint8_t base[4];
	int nops;
	struct op ops[MAX_OPS];
};

static void rect_rand(int *x, int *y, int *w, int *h)
{
	*w = 8 + rnd() % (tile / 2);
	*h = 8 + rnd() % (tile / 2);
	*x = rnd() % (tile - *w);
	*y = rnd() % (tile - *h);
}

static void colour_rand(uint8_t c[4])
{
	c[0] = rnd() & 0xff;
	c[1] = rnd() & 0xff;
	c[2] = rnd() & 0xff;
	c[3] = 255;
}

static void scene_make(struct scene *s, int iter)
{
	rng_state = 0x9E3779B9u * (uint32_t)(iter + 1);
	colour_rand(s->base);
	s->nops = 8 + rnd() % (MAX_OPS - 8);
	for (int i = 0; i < s->nops; i++) {
		struct op *o = &s->ops[i];
		o->kind = rnd() % 4;
		rect_rand(&o->x, &o->y, &o->w, &o->h);
		rect_rand(&o->cx, &o->cy, &o->cw, &o->ch);
		colour_rand(o->c0);
		colour_rand(o->c1);
	}
}

/* Reference rasteriser: rows in GL order (row 0 at the bottom). */
static void ref_fill(uint8_t *img, int x0, int y0, int x1, int y1,
		     const struct op *o)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > tile) x1 = tile;
	if (y1 > tile) y1 = tile;
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			uint8_t *p = img + (y * tile + x) * 4;
			if (o->kind == 1) {
				float t = ((float)x + 0.5f - (float)o->x) / (float)o->w;
				for (int k = 0; k < 3; k++)
					p[k] = (uint8_t)(o->c0[k] + (o->c1[k] - o->c0[k]) * t + 0.5f);
				p[3] = 255;
			} else {
				memcpy(p, o->c0, 4);
			}
		}
	}
}

static void scene_render_ref(const struct scene *s, uint8_t *img)
{
	for (int i = 0; i < tile * tile; i++)
		memcpy(img + i * 4, s->base, 4);
	for (int i = 0; i < s->nops; i++) {
		const struct op *o = &s->ops[i];
		int x0 = o->x, y0 = o->y, x1 = o->x + o->w, y1 = o->y + o->h;
		if (o->kind == 2 || o->kind == 3) {
			if (x0 < o->cx) x0 = o->cx;
			if (y0 < o->cy) y0 = o->cy;
			if (x1 > o->cx + o->cw) x1 = o->cx + o->cw;
			if (y1 > o->cy + o->ch) y1 = o->cy + o->ch;
			if (x1 <= x0 || y1 <= y0)
				continue;
		}
		ref_fill(img, x0, y0, x1, y1, o);
	}
}

/* ---- GL ---------------------------------------------------------------- */

static const char *vs_src =
	"#version 300 es\n"
	"in vec2 pos;\n"
	"uniform vec2 tilesz;\n"
	"void main() { gl_Position = vec4(pos / tilesz * 2.0 - 1.0, 0.0, 1.0); }\n";

static const char *fs_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform vec4 c0, c1;\n"
	"uniform vec2 grad; /* x origin, width; width 0 = solid */\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"  if (grad.y <= 0.0) { frag = c0; return; }\n"
	"  float t = (gl_FragCoord.x - grad.x) / grad.y;\n"
	"  frag = vec4(mix(c0.rgb, c1.rgb, t), 1.0);\n"
	"}\n";

static const char *tvs_src =
	"#version 300 es\n"
	"in vec2 pos;\n"
	"out vec2 uv;\n"
	"void main() { uv = pos; gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0); }\n";

static const char *tfs_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 uv;\n"
	"uniform sampler2D tex;\n"
	"out vec4 frag;\n"
	"void main() { frag = texture(tex, uv); }\n";

static GLuint make_prog(const char *vs, const char *fs)
{
	GLuint v = glCreateShader(GL_VERTEX_SHADER);
	GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
	GLint ok;
	char log[1024];

	glShaderSource(v, 1, &vs, NULL);
	glCompileShader(v);
	glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(v, sizeof log, NULL, log);
		fprintf(stderr, "vertex shader: %s\n", log);
		exit(2);
	}
	glShaderSource(f, 1, &fs, NULL);
	glCompileShader(f);
	glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(f, sizeof log, NULL, log);
		fprintf(stderr, "fragment shader: %s\n", log);
		exit(2);
	}
	GLuint p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "pos");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof log, NULL, log);
		fprintf(stderr, "link: %s\n", log);
		exit(2);
	}
	return p;
}

static GLuint vbo;

static void draw_rect(float x, float y, float w, float h)
{
	float v[8] = { x, y, x + w, y, x, y + h, x + w, y + h };

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void check_gl(const char *where)
{
	GLenum e = glGetError();

	if (e != GL_NO_ERROR) {
		fprintf(stderr, "GL error 0x%x at %s\n", e, where);
		exit(2);
	}
}

/* The paint: Skia's sequence for one tile. */
static void paint(GLuint prog, GLuint tex, const struct scene *s)
{
	GLuint fbo_res, fbo_ms = 0, rb_color = 0, rb_ds = 0, rb_ds1 = 0;

	glGenFramebuffers(1, &fbo_res);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_res);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, tex, 0);
	if (samples > 0) {
		glGenRenderbuffers(1, &rb_color);
		glBindRenderbuffer(GL_RENDERBUFFER, rb_color);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8,
						 tile, tile);
		glGenRenderbuffers(1, &rb_ds);
		glBindRenderbuffer(GL_RENDERBUFFER, rb_ds);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
						 GL_DEPTH24_STENCIL8, tile, tile);
		glGenFramebuffers(1, &fbo_ms);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo_ms);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					  GL_RENDERBUFFER, rb_color);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
					  GL_RENDERBUFFER, rb_ds);
	} else {
		glGenRenderbuffers(1, &rb_ds1);
		glBindRenderbuffer(GL_RENDERBUFFER, rb_ds1);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, tile, tile);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
					  GL_RENDERBUFFER, rb_ds1);
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "framebuffer incomplete (samples %d)\n", samples);
		exit(2);
	}
	glViewport(0, 0, tile, tile);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glUseProgram(prog);
	glUniform2f(glGetUniformLocation(prog, "tilesz"), (float)tile, (float)tile);
	GLint uc0 = glGetUniformLocation(prog, "c0");
	GLint uc1 = glGetUniformLocation(prog, "c1");
	GLint ug = glGetUniformLocation(prog, "grad");

	glClearColor(s->base[0] / 255.0f, s->base[1] / 255.0f,
		     s->base[2] / 255.0f, 1.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	for (int i = 0; i < s->nops; i++) {
		const struct op *o = &s->ops[i];

		glUniform4f(uc0, o->c0[0] / 255.0f, o->c0[1] / 255.0f,
			    o->c0[2] / 255.0f, 1.0f);
		glUniform4f(uc1, o->c1[0] / 255.0f, o->c1[1] / 255.0f,
			    o->c1[2] / 255.0f, 1.0f);
		glUniform2f(ug, (float)o->x, o->kind == 1 ? (float)o->w : 0.0f);
		if (o->kind == 2) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(o->cx, o->cy, o->cw, o->ch);
		} else if (o->kind == 3) {
			/* Skia's clip: mark the region in stencil, then draw
			 * through it. */
			glEnable(GL_STENCIL_TEST);
			glStencilFunc(GL_ALWAYS, 1, 0xff);
			glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
			glColorMask(0, 0, 0, 0);
			draw_rect((float)o->cx, (float)o->cy, (float)o->cw, (float)o->ch);
			glColorMask(1, 1, 1, 1);
			glStencilFunc(GL_EQUAL, 1, 0xff);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		}
		draw_rect((float)o->x, (float)o->y, (float)o->w, (float)o->h);
		if (o->kind == 2)
			glDisable(GL_SCISSOR_TEST);
		if (o->kind == 3) {
			/* Undo the clip mark, as Skia does for the next op. */
			glStencilFunc(GL_ALWAYS, 0, 0xff);
			glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
			glColorMask(0, 0, 0, 0);
			draw_rect((float)o->cx, (float)o->cy, (float)o->cw, (float)o->ch);
			glColorMask(1, 1, 1, 1);
			glDisable(GL_STENCIL_TEST);
		}
	}
	if (samples > 0) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_ms);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_res);
		glBlitFramebuffer(0, 0, tile, tile, 0, 0, tile, tile,
				  GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glFlush();
	check_gl("paint");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo_res);
	if (fbo_ms)
		glDeleteFramebuffers(1, &fbo_ms);
	if (rb_color)
		glDeleteRenderbuffers(1, &rb_color);
	if (rb_ds)
		glDeleteRenderbuffers(1, &rb_ds);
	if (rb_ds1)
		glDeleteRenderbuffers(1, &rb_ds1);
}

/* Read a texture back through a framebuffer, rows in GL order. */
static void read_tex(GLuint tex, uint8_t *out)
{
	GLuint fbo;

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, tex, 0);
	glReadPixels(0, 0, tile, tile, GL_RGBA, GL_UNSIGNED_BYTE, out);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	check_gl("readback");
}

/* The compositor's part: draw the tile into another texture. */
static void composite(GLuint tprog, GLuint tex, GLuint out_tex)
{
	GLuint fbo;

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, out_tex, 0);
	glViewport(0, 0, tile, tile);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glUseProgram(tprog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUniform1i(glGetUniformLocation(tprog, "tex"), 0);
	float v[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFlush();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	check_gl("composite");
}

/* ---- comparison ------------------------------------------------------- */

struct verdict {
	int bad;      /* mismatching pixels */
	int x0, y0, x1, y1; /* their bounding box */
	int stale;    /* of the bad, how many equal the tile's previous content */
	int fx, fy;   /* first bad pixel */
	uint8_t got[4], want[4];
};

static int near(const uint8_t *a, const uint8_t *b, int tol)
{
	for (int k = 0; k < 3; k++) {
		int d = (int)a[k] - (int)b[k];
		if (d < -tol || d > tol)
			return 0;
	}
	return 1;
}

static void compare(const uint8_t *got, const uint8_t *want, const uint8_t *prev,
		    struct verdict *v)
{
	memset(v, 0, sizeof *v);
	v->x0 = v->y0 = tile;
	v->x1 = v->y1 = -1;
	for (int y = 0; y < tile; y++) {
		for (int x = 0; x < tile; x++) {
			int i = (y * tile + x) * 4;

			if (near(got + i, want + i, 2))
				continue;
			if (!v->bad) {
				v->fx = x;
				v->fy = y;
				memcpy(v->got, got + i, 4);
				memcpy(v->want, want + i, 4);
			}
			v->bad++;
			if (x < v->x0) v->x0 = x;
			if (y < v->y0) v->y0 = y;
			if (x > v->x1) v->x1 = x;
			if (y > v->y1) v->y1 = y;
			if (prev && near(got + i, prev + i, 2))
				v->stale++;
		}
	}
}

static void report(const char *what, int iter, int slot, const struct verdict *v)
{
	printf("paint %d tile %d: %s WRONG: %d px in [%d,%d]-[%d,%d], %d of them = the tile's previous content; first at (%d,%d) got %02x%02x%02x want %02x%02x%02x\n",
	       iter, slot, what, v->bad, v->x0, v->y0, v->x1, v->y1, v->stale,
	       v->fx, v->fy, v->got[0], v->got[1], v->got[2], v->want[0],
	       v->want[1], v->want[2]);
}

/* ---- EGL ---------------------------------------------------------------- */

static EGLDisplay dpy;
static EGLConfig cfg;

static EGLContext make_ctx(EGLContext share)
{
	static const EGLint attrs31[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
					  EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
	static const EGLint attrs30[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
					  EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
	EGLContext c = eglCreateContext(dpy, cfg, share, attrs31);

	if (c == EGL_NO_CONTEXT)
		c = eglCreateContext(dpy, cfg, share, attrs30);
	if (c == EGL_NO_CONTEXT) {
		fprintf(stderr, "eglCreateContext failed: 0x%x\n", eglGetError());
		exit(2);
	}
	return c;
}

int main(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "n:s:t:p:d:2vS")) != -1) {
		switch (opt) {
		case 'n': iters = atoi(optarg); break;
		case 's': samples = atoi(optarg); break;
		case 't': tile = atoi(optarg); break;
		case 'p': pool_n = atoi(optarg); break;
		case 'd': devpath = optarg; break;
		case '2': two_ctx = 1; break;
		case 'v': verbose = 1; break;
		case 'S': surfaceless = 1; break;
		default:
			fprintf(stderr, "usage: gltile [-n paints] [-s samples] [-t tile] [-p pool] [-d node] [-2] [-S] [-v]\n");
			return 2;
		}
	}
	if (pool_n > 64)
		pool_n = 64;

	int fd = -1;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
#ifdef NO_GBM
	surfaceless = 1;
	(void)devpath;
#else
	struct gbm_device *gbm = NULL;
	if (!surfaceless) {
		fd = open(devpath, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			perror(devpath);
			return 2;
		}
		gbm = gbm_create_device(fd);
		if (!gbm) {
			fprintf(stderr, "gbm_create_device failed\n");
			return 2;
		}
		if (get_platform_display)
			dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
		else
			dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
	}
#endif
	if (surfaceless) {
		if (!get_platform_display) {
			fprintf(stderr, "no eglGetPlatformDisplayEXT\n");
			return 2;
		}
		dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
					   EGL_DEFAULT_DISPLAY, NULL);
	}
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "EGL display (%s) failed: 0x%x\n",
			surfaceless ? "surfaceless" : "GBM", eglGetError());
		return 2;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint cattrs[] = { EGL_SURFACE_TYPE, 0,
					 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
					 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
					 EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
					 EGL_NONE };
	EGLint ncfg = 0;
	if (!eglChooseConfig(dpy, cattrs, &cfg, 1, &ncfg) || ncfg < 1) {
		/* A surfaceless context needs no config at all where that is
		 * supported; try that. */
		cfg = EGL_NO_CONFIG_KHR;
	}
	EGLContext ctx = make_ctx(EGL_NO_CONTEXT);
	EGLContext ctx2 = two_ctx ? make_ctx(ctx) : EGL_NO_CONTEXT;
	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "eglMakeCurrent (surfaceless) failed: 0x%x\n",
			eglGetError());
		return 2;
	}
	printf("gltile: %s, %s, %s\n", glGetString(GL_RENDERER),
	       glGetString(GL_VERSION), eglQueryString(dpy, EGL_VENDOR));
	if (samples > 0) {
		GLint maxs = 0;
		glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_SAMPLES, 1, &maxs);
		if (maxs < samples) {
			printf("gltile: RGBA8 supports %d samples at most; using that\n",
			       maxs);
			samples = maxs;
		}
	}
	printf("gltile: %d paints, tile %dx%d, pool %d, %d samples%s\n", iters,
	       tile, tile, pool_n, samples,
	       two_ctx ? ", compositor in a second context" : "");

	GLuint prog = make_prog(vs_src, fs_src);
	GLuint tprog = make_prog(tvs_src, tfs_src);
	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);

	GLuint pool[64], out_tex;
	glGenTextures(pool_n, pool);
	for (int i = 0; i < pool_n; i++) {
		glBindTexture(GL_TEXTURE_2D, pool[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tile, tile, 0, GL_RGBA,
			     GL_UNSIGNED_BYTE, NULL);
	}
	glGenTextures(1, &out_tex);
	glBindTexture(GL_TEXTURE_2D, out_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tile, tile, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	check_gl("setup");

	size_t bytes = (size_t)tile * tile * 4;
	uint8_t *want = malloc(bytes), *got = malloc(bytes);
	uint8_t **prev = calloc(pool_n, sizeof *prev);
	GLuint vao2 = 0, vbo2 = 0, tprog2 = 0;

	if (two_ctx) {
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx2);
		glGenVertexArrays(1, &vao2);
		glBindVertexArray(vao2);
		glGenBuffers(1, &vbo2);
		tprog2 = make_prog(tvs_src, tfs_src);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
	}

	int failures = 0, tile_bad = 0, comp_bad = 0;
	struct scene s;

	for (int it = 0; it < iters; it++) {
		int slot = it % pool_n;
		GLuint tex = pool[slot];

		/* Turnover: a tile texture is released and a new one
		 * allocated now and then, as the pool does. */
		if (it && it % 37 == 0) {
			glDeleteTextures(1, &tex);
			glGenTextures(1, &tex);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tile, tile, 0,
				     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			pool[slot] = tex;
			free(prev[slot]);
			prev[slot] = NULL;
		}
		scene_make(&s, it);
		scene_render_ref(&s, want);
		paint(prog, tex, &s);

		struct verdict v;
		int bad_here = 0;

		read_tex(tex, got);
		compare(got, want, prev[slot], &v);
		if (v.bad) {
			report("tile", it, slot, &v);
			tile_bad++;
			bad_here = 1;
		}
		if (two_ctx) {
			/* Hand the tile to the compositor the way WebKit does:
			 * a fence in the painter, waited for in the compositor. */
			GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			glFlush();
			eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx2);
			glBindVertexArray(vao2);
			glWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
			GLuint save = vbo;
			vbo = vbo2;
			composite(tprog2, tex, out_tex);
			vbo = save;
			read_tex(out_tex, got);
			glDeleteSync(sync);
			eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
			glBindVertexArray(vao);
		} else {
			composite(tprog, tex, out_tex);
			read_tex(out_tex, got);
		}
		compare(got, want, prev[slot], &v);
		if (v.bad) {
			report("composited copy", it, slot, &v);
			comp_bad++;
			bad_here = 1;
		}
		if (bad_here)
			failures++;
		else if (verbose)
			printf("paint %d tile %d: ok (%d ops)\n", it, slot, s.nops);
		if (!prev[slot])
			prev[slot] = malloc(bytes);
		memcpy(prev[slot], want, bytes);
	}
	printf("gltile: %d paints, %d failed (%d wrong tiles, %d wrong composited copies)\n",
	       iters, failures, tile_bad, comp_bad);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	if (two_ctx)
		eglDestroyContext(dpy, ctx2);
	eglTerminate(dpy);
#ifndef NO_GBM
	if (gbm)
		gbm_device_destroy(gbm);
#endif
	if (fd >= 0)
		close(fd);
	return failures ? 1 : 0;
}
