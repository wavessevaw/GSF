// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA interpolation correctness test and benchmark.
//
// Renders analytic scenes at continuous time, so the true intermediate frame
// at any phase is known. Runs the production pass plan and SPIR-V on a real
// Vulkan device (lavapipe in CI; RADV when run on the Steam Deck) and checks:
//   * generated frames are closer to the true intermediate than a blend or a
//     duplicate of the nearest frame (genuine temporal interpolation),
//   * static content and HUD stay exact, scene cuts are never blended,
//   * x3 phases 1/3 and 2/3 differ from each other and from both sources,
//   * the frame ring handles a running sequence.
// Writes a JSON report; GPU pass durations come from Vulkan timestamps and are
// only meaningful on the device they were measured on.
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kfg_vk.h"

typedef struct scene {
	const char *name;
	int kind;
	double vx, vy;          // background motion, pixels per source frame
	bool dynamic;
	bool cut;
} scene;

enum { S_PAN, S_OBJECTS, S_OCCLUSION, S_STATIC, S_CUT, S_FASTPAN, S_STAIRS };

static const scene SCENES[] = {
	{ "pan", S_PAN, 24, 6, true, false },
	{ "fast_pan", S_FASTPAN, 72, 0, true, false },
	{ "objects_hud", S_OBJECTS, 0, 0, true, false },
	{ "occlusion", S_OCCLUSION, -16, 0, true, false },
	{ "static", S_STATIC, 0, 0, false, false },
	{ "scene_cut", S_CUT, 0, 0, false, true },
	{ "stairs", S_STAIRS, 18, 10, true, false },
};

static double hash2(int x, int y, int seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (double)(h & 0xffffff) / 16777215.0;
}

static double vnoise(double x, double y, int seed)
{
	int xi = (int)floor(x), yi = (int)floor(y);
	double fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	double a = hash2(xi, yi, seed), b = hash2(xi + 1, yi, seed), c = hash2(xi, yi + 1, seed), d = hash2(xi + 1, yi + 1, seed);
	return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

// Textured background: multi-octave noise with a brick grid, in [0,1]^3.
static void background(double x, double y, int seed, double rgb[3])
{
	double n = 0.5 * vnoise(x / 37.0, y / 37.0, seed) + 0.3 * vnoise(x / 11.0, y / 11.0, seed + 1) + 0.2 * vnoise(x / 4.0, y / 4.0, seed + 2);
	double bx = fmod(x + (fmod(floor(y / 40.0), 2.0) ? 30.0 : 0.0), 60.0), by = fmod(y, 40.0);
	if (bx < 0) bx += 60.0;
	if (by < 0) by += 40.0;
	double mortar = (bx < 3.0 || by < 3.0) ? 0.35 : 1.0;
	rgb[0] = (0.25 + 0.6 * n) * mortar;
	rgb[1] = (0.20 + 0.5 * vnoise(x / 23.0 + 7, y / 23.0, seed + 3)) * mortar;
	rgb[2] = (0.15 + 0.7 * (1 - n)) * mortar;
}

static bool hud(double x, double y, int w, int h, double rgb[3])
{
	// Text-like high-contrast glyph block, top left, and a status bar at the bottom.
	if (x >= 40 && x < 360 && y >= 30 && y < 70) {
		int cx = (int)((x - 40) / 8), cy = (int)((y - 30) / 8);
		bool on = hash2(cx, cy, 99) > 0.45 && fmod(x - 40, 8) < 6 && fmod(y - 30, 8) < 6;
		rgb[0] = rgb[1] = rgb[2] = on ? 0.95 : 0.05;
		return true;
	}
	if (y >= h - 50 && y < h - 30 && x >= 60 && x < w - 60) {
		rgb[0] = 0.9;
		rgb[1] = 0.75;
		rgb[2] = 0.1;
		return true;
	}
	return false;
}

// Stairs and a railing: treads repeat every 14 pixels and the camera moves 18 per source
// frame, so a wrong match 4 pixels off fits almost as well as the true motion (the aperture
// problem behind flickering stairs and straight lines in games).
static void stairs(double x, double y, double rgb[3])
{
	const double n = 0.06 * vnoise(x / 9.0, y / 9.0, 5);
	double ty = fmod(y + 0.35 * x, 14.0);
	if (ty < 0) ty += 14.0;
	double v = ty < 2.0 ? 0.92 : (ty < 3.5 ? 0.12 : 0.45 + 0.25 * ty / 14.0);
	// Railing: a thin straight handrail and posts.
	double ry = fmod(y + 0.35 * x, 112.0), rx = fmod(x, 46.0);
	if (ry < 0) ry += 112.0;
	if (rx < 0) rx += 46.0;
	if ((ry > 40 && ry < 41.6) || (rx < 1.5 && ry > 41.6 && ry < 80))
		v = 0.05;
	rgb[0] = v + n;
	rgb[1] = 0.95 * v + n;
	rgb[2] = 0.85 * v + n;
}

static void scene_pixel(const scene *s, double t, double x, double y, int w, int h, double rgb[3])
{
	switch (s->kind) {
	case S_STAIRS:
		stairs(x - s->vx * t, y - s->vy * t, rgb);
		return;
	case S_STATIC:
		background(x, y, 1, rgb);
		return;
	case S_CUT:
		background(x, y, t < 0.5 ? 1 : 50, rgb);
		return;
	case S_PAN:
	case S_FASTPAN:
		background(x - s->vx * t, y - s->vy * t, 1, rgb);
		return;
	case S_OCCLUSION: {
		background(x - s->vx * t, y - s->vy * t, 1, rgb);
		double ox = 300 + 40 * t, oy = 260;
		if (x >= ox && x < ox + 160 && y >= oy && y < oy + 160)
			background((x - ox) * 1.3 + 2000, (y - oy) * 1.3, 7, rgb);
		return;
	}
	case S_OBJECTS:
	default: {
		if (hud(x, y, w, h, rgb))
			return;
		background(x, y, 1, rgb);
		double ox = 300 + 36 * t, oy = 320 - 12 * t;
		if (x >= ox && x < ox + 120 && y >= oy && y < oy + 120) {
			background((x - ox) * 1.5 + 5000, (y - oy) * 1.5, 11, rgb);
			return;
		}
		double dx = x - (850 - 28 * t), dy = y - (450 + 20 * t);
		if (dx * dx + dy * dy < 55 * 55) {
			double a = atan2(dy, dx);
			double stripe = fmod(a * 6 / 3.14159265 + 12, 2.0) < 1.0 ? 1.0 : 0.3;
			rgb[0] = 0.9 * stripe;
			rgb[1] = 0.3 * stripe;
			rgb[2] = 0.3;
		}
		return;
	}
	}
}

static void render(const scene *s, double t, int w, int h, uint8_t *out)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double acc[3] = { 0, 0, 0 };
			for (int sy = 0; sy < 2; sy++)
				for (int sx = 0; sx < 2; sx++) {
					double c[3];
					scene_pixel(s, t, x + 0.25 + 0.5 * sx, y + 0.25 + 0.5 * sy, w, h, c);
					for (int i = 0; i < 3; i++)
						acc[i] += c[i] * 0.25;
				}
			uint8_t *p = out + 4 * ((size_t)y * w + x);
			for (int i = 0; i < 3; i++) {
				double v = acc[i] * 255.0 + 0.5;
				p[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
			}
			p[3] = 255;
		}
}

static double psnr_region(const uint8_t *a, const uint8_t *b, int w, int h, int x0, int y0, int x1, int y1)
{
	double se = 0;
	long n = 0;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			for (int c = 0; c < 3; c++) {
				double d = (double)a[4 * ((size_t)y * w + x) + c] - b[4 * ((size_t)y * w + x) + c];
				se += d * d;
				n++;
			}
	if (!n)
		return 0;
	double mse = se / (double)n;
	return mse <= 1e-12 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

static double mean_abs(const uint8_t *a, const uint8_t *b, int w, int h, int x0, int y0, int x1, int y1)
{
	(void)h;
	double s = 0;
	long n = 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			for (int c = 0; c < 3; c++) {
				s += fabs((double)a[4 * ((size_t)y * w + x) + c] - b[4 * ((size_t)y * w + x) + c]);
				n++;
			}
	return n ? s / (double)n : 0;
}

// Temporal error: how much the change from the previous output to this one differs from the
// true change, per pixel and channel (flicker and shimmer show up here, a static blur does not).
static double temporal_err(const uint8_t *o, const uint8_t *op, const uint8_t *t, const uint8_t *tp, int w, int h)
{
	double sum = 0;
	for (int i = 0; i < w * h; i++)
		for (int c = 0; c < 3; c++) {
			const int k = i * 4 + c;
			sum += fabs((double)(o[k] - op[k]) - (double)(t[k] - tp[k]));
		}
	return sum / (3.0 * w * h);
}

static int max_abs(const uint8_t *a, const uint8_t *b, size_t n)
{
	int m = 0;
	for (size_t i = 0; i < n; i++) {
		if ((i & 3) == 3)
			continue;
		int d = abs((int)a[i] - (int)b[i]);
		if (d > m)
			m = d;
	}
	return m;
}

static void write_ppm(const char *dir, const char *name, const uint8_t *rgba, int w, int h)
{
	if (!dir)
		return;
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (size_t i = 0; i < (size_t)w * h; i++)
		fwrite(rgba + 4 * i, 1, 3, f);
	fclose(f);
}

// ---- engine resources ---------------------------------------------------------------------
typedef struct engine {
	kfg_vk *vk;
	uint32_t w, h;
	kfg_vk_image color[KFG_RING];
	kfg_vk_image luma[KFG_RING][KFG_LEVELS + 1];
	kfg_vk_image flow[KFG_RING][KFG_LEVELS + 1];
	kfg_vk_image stats[KFG_RING];
	kfg_vk_image select;
	kfg_vk_image output;
} engine;

static const kfg_vk_image *resolve(void *user, kfg_image_ref r)
{
	engine *e = user;
	switch (r.role) {
	case KFG_ROLE_COLOR: return &e->color[r.ring];
	case KFG_ROLE_LUMA: return &e->luma[r.ring][r.level];
	case KFG_ROLE_FLOW: return &e->flow[r.ring][r.level];
	case KFG_ROLE_STATS: return &e->stats[r.ring];
	case KFG_ROLE_SELECT: return &e->select;
	case KFG_ROLE_OUTPUT: return &e->output;
	default: return NULL;
	}
}

static int engine_init(engine *e, kfg_vk *vk, uint32_t w, uint32_t h)
{
	memset(e, 0, sizeof(*e));
	e->vk = vk;
	e->w = w;
	e->h = h;
	for (uint32_t r = 0; r < KFG_RING; r++) {
		if (kfg_vk_image_create(vk, &e->color[r], VK_FORMAT_R8G8B8A8_UNORM, w, h))
			return -1;
		for (uint32_t l = 1; l <= KFG_LEVELS; l++) {
			uint32_t sz[2];
			kfg_level_size(w, h, l, sz);
			if (kfg_vk_image_create(vk, &e->luma[r][l], kfg_vk_format(KFG_FORMAT_R8), sz[0], sz[1]))
				return -1;
			if (l >= 2 && kfg_vk_image_create(vk, &e->flow[r][l], kfg_vk_format(KFG_FORMAT_RGBA16F), sz[0], sz[1]))
				return -1;
		}
		if (kfg_vk_image_create(vk, &e->stats[r], kfg_vk_format(KFG_FORMAT_RGBA16F), 1, 1))
			return -1;
	}
	uint32_t bs[2];
	kfg_select_size(w, h, bs);
	if (kfg_vk_image_create(vk, &e->select, kfg_vk_format(KFG_FORMAT_RGBA16F), bs[0], bs[1]))
		return -1;
	return kfg_vk_image_create(vk, &e->output, kfg_vk_format(KFG_FORMAT_RGBA8), w, h);
}

static void engine_destroy(engine *e)
{
	for (uint32_t r = 0; r < KFG_RING; r++) {
		kfg_vk_image_destroy(e->vk, &e->color[r]);
		for (uint32_t l = 0; l <= KFG_LEVELS; l++) {
			kfg_vk_image_destroy(e->vk, &e->luma[r][l]);
			kfg_vk_image_destroy(e->vk, &e->flow[r][l]);
		}
		kfg_vk_image_destroy(e->vk, &e->stats[r]);
	}
	kfg_vk_image_destroy(e->vk, &e->select);
	kfg_vk_image_destroy(e->vk, &e->output);
}

static int run_plan(engine *e, const kfg_plan *p, double *times, uint32_t *ntimes)
{
	kfg_vk_begin(e->vk);
	if (kfg_vk_record(e->vk, p, resolve, e))
		return -1;
	if (kfg_vk_submit_wait(e->vk))
		return -1;
	if (times && ntimes)
		*ntimes = kfg_vk_pass_times(e->vk, times, KFG_MAX_PASSES);
	return 0;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	const char *spv = "build";
	const char *out_dir = NULL;
	const char *json_path = NULL;
	const char *only = NULL;   // run one scene (tuning); skips the sequence and stairs checks
	int w = 1280, h = 800, bench = 0;
	bool validation = getenv("KFG_VALIDATION") != NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--spv") && i + 1 < argc) spv = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
		else if (!strcmp(argv[i], "--json") && i + 1 < argc) json_path = argv[++i];
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
		else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--scene") && i + 1 < argc) only = argv[++i];
		else {
			fprintf(stderr, "usage: kfg_vktest [--spv DIR] [--size WxH] [--out DIR] [--json FILE] [--bench N] [--scene NAME]\n");
			return 2;
		}
	}
	kfg_vk vk;
	if (kfg_vk_init(&vk, spv, validation)) {
		fprintf(stderr, "Vulkan init failed\n");
		return 1;
	}
	engine e;
	if (engine_init(&e, &vk, (uint32_t)w, (uint32_t)h)) {
		fprintf(stderr, "engine init failed\n");
		return 1;
	}
	kfg_tuning tune = kfg_default_tuning();
	size_t nbytes = (size_t)w * h * 4;
	uint8_t *A = malloc(nbytes), *B = malloc(nbytes), *T = malloc(nbytes), *G = malloc(nbytes), *L = malloc(nbytes),
		*D = malloc(nbytes), *G2 = malloc(nbytes);
	FILE *js = json_path ? fopen(json_path, "w") : NULL;
	if (js)
		fprintf(js, "{\n \"device\": \"%s\",\n \"driver\": \"%s\",\n \"size\": [%d, %d],\n \"scenes\": [\n", vk.device_name, vk.driver_info, w, h);
	int failures = 0;
	printf("device: %s (%s)\n", vk.device_name, vk.driver_info);

	const double phases[] = { 1.0 / 3.0, 2.0 / 3.0, 0.5 };
	for (size_t si = 0; si < sizeof(SCENES) / sizeof(SCENES[0]); si++) {
		const scene *s = &SCENES[si];
		if (only && strcmp(only, s->name))
			continue;
		render(s, 0.0, w, h, A);
		render(s, 1.0, w, h, B);
		kfg_vk_upload_rgba8(&vk, &e.color[0], A);
		kfg_vk_upload_rgba8(&vk, &e.color[1], B);
		kfg_plan p;
		kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, 1, 0, 0, 0, &tune); // pyramid of A (ring 0)
		run_plan(&e, &p, NULL, NULL);
		kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, 0, 1, 1, 1, &tune); // pair A(0)->B(1), pair ring 1
		run_plan(&e, &p, NULL, NULL);
		float st[4];
		kfg_vk_download_rgba16f(&vk, &e.stats[1], st);
		if (getenv("KFG_FLOWDUMP")) {
			// Tuning aid: the most common forward vectors per level (pixels, rounded).
			for (int l = KFG_LEVELS; l >= 2; l--) {
				const kfg_vk_image *fi = &e.flow[1][l];
				float *fl = malloc((size_t)fi->width * fi->height * 4 * sizeof(float));
				kfg_vk_download_rgba16f(&vk, fi, fl);
				int best[3] = {0}, bx[3] = {0}, by[3] = {0};
				static int hist[129][129];
				memset(hist, 0, sizeof(hist));
				for (uint32_t i = 0; i < fi->width * fi->height; i++) {
					int x = (int)lrintf(fl[i * 4]) + 64, y = (int)lrintf(fl[i * 4 + 1]) + 64;
					if (x >= 0 && x <= 128 && y >= 0 && y <= 128)
						hist[y][x]++;
				}
				for (int y = 0; y <= 128; y++)
					for (int x = 0; x <= 128; x++)
						for (int k = 0; k < 3; k++)
							if (hist[y][x] > best[k]) {
								for (int j = 2; j > k; j--) { best[j] = best[j - 1]; bx[j] = bx[j - 1]; by[j] = by[j - 1]; }
								best[k] = hist[y][x]; bx[k] = x - 64; by[k] = y - 64;
								break;
							}
				printf("  level %d: (%d,%d) %.0f%%  (%d,%d) %.0f%%  (%d,%d) %.0f%%\n", l,
				       bx[0], by[0], 100.0 * best[0] / (fi->width * fi->height), bx[1], by[1], 100.0 * best[1] / (fi->width * fi->height),
				       bx[2], by[2], 100.0 * best[2] / (fi->width * fi->height));
				free(fl);
			}
		}
		if (js)
			fprintf(js, "  {\"scene\": \"%s\", \"dynamic\": %s, \"cut_flag\": %.0f, \"bad_fraction\": %.4f, \"phases\": [\n",
				s->name, s->dynamic ? "true" : "false", st[0], st[1]);
		printf("%-12s cut=%.0f bad=%.3f mc=%.4f zero=%.4f\n", s->name, st[0], st[1], st[2], st[3]);
		if (s->cut && st[0] < 0.5) {
			printf("  FAIL: scene cut not detected\n");
			failures++;
		}
		if (!s->cut && st[0] > 0.5) {
			printf("  FAIL: false scene cut\n");
			failures++;
		}
		for (size_t pi = 0; pi < 3; pi++) {
			double t = phases[pi];
			kfg_plan_generate(&p, (uint32_t)w, (uint32_t)h, 0, 1, 1, (float)t, 0);
			run_plan(&e, &p, NULL, NULL);
			kfg_vk_download_rgba8(&vk, &e.output, G);
			if (pi == 0)
				memcpy(G2, G, nbytes);
			render(s, t, w, h, T);
			for (size_t i = 0; i < nbytes; i++) {
				L[i] = (uint8_t)lrint((1 - t) * A[i] + t * B[i]);
				D[i] = t < 0.5 ? A[i] : B[i];
			}
			double pg = psnr_region(G, T, w, h, 0, 0, w, h);
			double pl = psnr_region(L, T, w, h, 0, 0, w, h);
			double pd = psnr_region(D, T, w, h, 0, 0, w, h);
			double ma = mean_abs(G, A, w, h, 0, 0, w, h), mb = mean_abs(G, B, w, h, 0, 0, w, h);
			// Region of moving objects (objects scene) for a local measure.
			double og = pg, ol = pl, od = pd;
			if (s->kind == S_OBJECTS) {
				og = psnr_region(G, T, w, h, 280, 280, 480, 460);
				ol = psnr_region(L, T, w, h, 280, 280, 480, 460);
				od = psnr_region(D, T, w, h, 280, 280, 480, 460);
			} else if (s->kind == S_OCCLUSION) {
				og = psnr_region(G, T, w, h, 280, 240, 520, 440);
				ol = psnr_region(L, T, w, h, 280, 240, 520, 440);
				od = psnr_region(D, T, w, h, 280, 240, 520, 440);
			}
			double hud_err = s->kind == S_OBJECTS ? mean_abs(G, T, w, h, 40, 30, 360, 70) : 0.0;
			int maxd = max_abs(G, s->cut ? D : A, nbytes);
			bool ok = true;
			const char *why = "";
			if (s->dynamic) {
				if (!(pg >= pl + 3.0 && pg >= pd + 3.0)) { ok = false; why = "not better than blend/duplicate by 3 dB"; }
				if (!(og >= ol + 2.0 && og >= od + 2.0)) { ok = false; why = "moving region not better than blend/duplicate by 2 dB"; }
				if (!(ma > 0.5 && mb > 0.5)) { ok = false; why = "output equals a source frame"; }
				if (hud_err > 2.0) { ok = false; why = "HUD distorted"; }
			} else if (s->cut) {
				if (maxd > 0) { ok = false; why = "scene cut blended"; }
			} else {
				if (maxd > 1) { ok = false; why = "static scene changed"; }
			}
			if (!ok)
				failures++;
			printf("  t=%.3f gen=%.2fdB blend=%.2fdB dup=%.2fdB region gen/blend/dup=%.2f/%.2f/%.2f |G-A|=%.2f |G-B|=%.2f hud=%.2f %s%s\n",
			       t, pg, pl, pd, og, ol, od, ma, mb, hud_err, ok ? "PASS" : "FAIL: ", why);
			if (js)
				fprintf(js, "   {\"t\": %.6f, \"psnr_gen\": %.3f, \"psnr_blend\": %.3f, \"psnr_dup\": %.3f, \"region_gen\": %.3f, \"region_blend\": %.3f, \"region_dup\": %.3f, \"mean_abs_vs_A\": %.3f, \"mean_abs_vs_B\": %.3f, \"hud_err\": %.3f, \"pass\": %s}%s\n",
					t, pg, pl, pd, og, ol, od, ma, mb, hud_err, ok ? "true" : "false", pi < 2 ? "," : "");
			char name[128];
			snprintf(name, sizeof(name), "%s_t%03d_gen", s->name, (int)lrint(t * 1000));
			write_ppm(out_dir, name, G, w, h);
			snprintf(name, sizeof(name), "%s_t%03d_truth", s->name, (int)lrint(t * 1000));
			write_ppm(out_dir, name, T, w, h);
			if (pi == 1 && s->dynamic) {
				// x3: the two intermediates must differ from each other.
				double d12 = mean_abs(G, G2, w, h, 0, 0, w, h);
				if (d12 < 0.5) {
					printf("  FAIL: x3 intermediates identical\n");
					failures++;
				}
			}
		}
		if (js)
			fprintf(js, "  ]}%s\n", si + 1 < sizeof(SCENES) / sizeof(SCENES[0]) ? "," : "");
	}
	if (js)
		fprintf(js, " ],\n");

	// Running sequence through the ring: 8 frames of the objects scene, x3 outputs per pair.
	if (!only) {
		const scene *s = &SCENES[2];
		int seq_fail = 0;
		for (int f = 0; f < 8; f++) {
			uint32_t fb = (uint32_t)f % KFG_RING, fa = (uint32_t)(f + KFG_RING - 1) % KFG_RING;
			render(s, (double)f, w, h, B);
			kfg_vk_upload_rgba8(&vk, &e.color[fb], B);
			kfg_plan p;
			kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, f > 0, &tune);
			run_plan(&e, &p, NULL, NULL);
			if (f == 0)
				continue;
			for (int j = 1; j < 3; j++) {
				double t = j / 3.0;
				kfg_plan_generate(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, (float)t, 0);
				run_plan(&e, &p, NULL, NULL);
				kfg_vk_download_rgba8(&vk, &e.output, G);
				render(s, f - 1 + t, w, h, T);
				render(s, f - 1 + (t < 0.5 ? 0.0 : 1.0), w, h, D);
				double pg = psnr_region(G, T, w, h, 0, 0, w, h), pd = psnr_region(D, T, w, h, 0, 0, w, h);
				if (pg < pd + 3.0)
					seq_fail++;
			}
		}
		printf("sequence: %s (%d bad outputs of 14)\n", seq_fail ? "FAIL" : "PASS", seq_fail);
		failures += seq_fail;
		if (js)
			fprintf(js, " \"sequence_bad_outputs\": %d,\n", seq_fail);
	}

	// Temporal stability on the stairs: 7 frames at x3, every output compared with the one
	// before it. Informative: printed and written to the JSON for tuning.
	{
		const scene *s = &SCENES[6];
		uint8_t *O = malloc(nbytes), *Op = malloc(nbytes), *Tp = malloc(nbytes), *Dp = malloc(nbytes);
		double te_gen = 0, te_dup = 0, ps_gen = 0, ps_dup = 0;
		int n_te = 0, n_ps = 0;
		bool have_prev = false;
		for (int f = 0; f < 7; f++) {
			uint32_t fb = (uint32_t)f % KFG_RING, fa = (uint32_t)(f + KFG_RING - 1) % KFG_RING;
			render(s, (double)f, w, h, B);
			kfg_vk_upload_rgba8(&vk, &e.color[fb], B);
			kfg_plan p;
			kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, f > 0, &tune);
			run_plan(&e, &p, NULL, NULL);
			if (f == 0)
				continue;
			for (int j = 0; j < 3; j++) {
				double t = j / 3.0;
				render(s, f - 1 + t, w, h, T);
				render(s, f - 1 + (t < 0.5 ? 0.0 : 1.0), w, h, D);
				if (j == 0) {
					render(s, f - 1, w, h, O);
				} else {
					kfg_plan_generate(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, (float)t, 0);
					run_plan(&e, &p, NULL, NULL);
					kfg_vk_download_rgba8(&vk, &e.output, O);
					ps_gen += psnr_region(O, T, w, h, 0, 0, w, h);
					ps_dup += psnr_region(D, T, w, h, 0, 0, w, h);
					n_ps++;
				}
				if (have_prev) {
					te_gen += temporal_err(O, Op, T, Tp, w, h);
					te_dup += temporal_err(D, Dp, T, Tp, w, h);
					n_te++;
				}
				memcpy(Op, O, nbytes);
				memcpy(Tp, T, nbytes);
				memcpy(Dp, D, nbytes);
				have_prev = true;
			}
		}
		printf("stairs temporal: gen %.3f, duplicate %.3f (mean |change error| per channel); psnr gen %.2f dB, dup %.2f dB\n",
		       te_gen / n_te, te_dup / n_te, ps_gen / n_ps, ps_dup / n_ps);
		if (js)
			fprintf(js, " \"stairs_temporal\": {\"gen\": %.4f, \"dup\": %.4f, \"psnr_gen\": %.3f, \"psnr_dup\": %.3f},\n",
				te_gen / n_te, te_dup / n_te, ps_gen / n_ps, ps_dup / n_ps);
		free(O); free(Op); free(Tp); free(Dp);
	}

	// Benchmark: analysis of one pair + two generated outputs (x3), GPU timestamps per pass.
	if (bench > 0 && vk.timestamps) {
		double samples[KFG_MAX_PASSES * 2][512];
		char labels[KFG_MAX_PASSES * 2][32];
		uint32_t npass = 0;
		int reps = bench > 512 ? 512 : bench;
		const scene *s = &SCENES[2];
		render(s, 0.0, w, h, A);
		render(s, 1.0, w, h, B);
		kfg_vk_upload_rgba8(&vk, &e.color[0], A);
		kfg_vk_upload_rgba8(&vk, &e.color[1], B);
		for (int r = 0; r < reps + 3; r++) {
			kfg_plan pa, g1, g2;
			kfg_plan_analyze(&pa, (uint32_t)w, (uint32_t)h, 0, 1, 1, 1, &tune);
			kfg_plan_generate(&g1, (uint32_t)w, (uint32_t)h, 0, 1, 1, 1.0f / 3.0f, 0);
			kfg_plan_generate(&g2, (uint32_t)w, (uint32_t)h, 0, 1, 1, 2.0f / 3.0f, 0);
			kfg_vk_begin(&vk);
			kfg_vk_record(&vk, &pa, resolve, &e);
			kfg_vk_record(&vk, &g1, resolve, &e);
			kfg_vk_record(&vk, &g2, resolve, &e);
			kfg_vk_submit_wait(&vk);
			double t[KFG_MAX_PASSES * 2];
			uint32_t n = kfg_vk_pass_times(&vk, t, KFG_MAX_PASSES * 2);
			if (r < 3)
				continue; // warm-up
			npass = n;
			for (uint32_t i = 0; i < n; i++) {
				samples[i][r - 3] = t[i];
				const kfg_plan *src = i < pa.n ? &pa : i < pa.n + g1.n ? &g1 : &g2;
				uint32_t k = i < pa.n ? i : i < pa.n + g1.n ? i - pa.n : i - pa.n - g1.n;
				snprintf(labels[i], sizeof(labels[i]), "%s%s", src == &pa ? "" : src == &g1 ? "g1:" : "g2:", src->pass[k].label);
			}
		}
		double analysis = 0, gen = 0;
		printf("benchmark (%d reps, %s): median GPU ms per pass\n", reps, vk.device_name);
		if (js)
			fprintf(js, " \"bench\": {\"reps\": %d, \"passes\": [", reps);
		for (uint32_t i = 0; i < npass; i++) {
			qsort(samples[i], (size_t)reps, sizeof(double), cmp_double);
			double med = samples[i][reps / 2] / 1e6, p99 = samples[i][(reps * 99) / 100] / 1e6;
			printf("  %-12s %.3f (p99 %.3f)\n", labels[i], med, p99);
			if (strncmp(labels[i], "g", 1))
				analysis += med;
			else
				gen += med;
			if (js)
				fprintf(js, "%s{\"pass\": \"%s\", \"median_ms\": %.4f, \"p99_ms\": %.4f}", i ? ", " : "", labels[i], med, p99);
		}
		printf("  analysis %.3f ms, two outputs %.3f ms\n", analysis, gen);
		if (js)
			fprintf(js, "], \"analysis_ms\": %.4f, \"two_outputs_ms\": %.4f},\n", analysis, gen);
	}
	if (js) {
		fprintf(js, " \"failures\": %d\n}\n", failures);
		fclose(js);
	}
	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	engine_destroy(&e);
	kfg_vk_destroy(&vk);
	free(A); free(B); free(T); free(G); free(L); free(D); free(G2);
	return failures ? 1 : 0;
}
