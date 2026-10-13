/*
 * Copyright (c) 2024 Jiri Svoboda
 * Copyright (c) 2013 Martin Decky
 *  * Copyright (c) 2026 Jocic Ivica
 * Prilagođeno za SrBin OS - bmp.c komponenta Slikovnice
 */

 #include <errno.h>
 #include <gfximage/tga.h>
 #include <stdbool.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <str.h>
 #include <str_error.h>
 #include <task.h>
 #include <ui/filedialog.h>
 #include <ui/image.h>
 #include <ui/ui.h>
 #include <ui/wdecor.h>
 #include <ui/window.h>
 #include <vfs/vfs.h>
 #include <jpeglib.h>
 #include <src/webp/decode.h>
 #include <setjmp.h>
 #include <nanosvgrast.h>
#include <nanosvg.h>
 
 #define USESTB
 #define STB_IMAGE_IMPLEMENTATION
 #include "stb_image.h"
 

 #include "bmp.h"
 
 #define NAME  "slikovnica"
 #define HELP_APP "/app/test_html"

 static const char srbin_verzija[] = "SRBINOS_VER:2.0.0";
 const char *__attribute__((used)) srbin_verzija_ptr = srbin_verzija;
 
 
 /* 
  * LOKALNI RAM KEŠ ZA SLIKOVNICU (Logika iz Pauka)
  * Definišemo strukture ovde da kompajler ne bi javljao 'undeclared'
  */
 typedef struct {
	 char *key;
	 unsigned char *data;
	 int width;
	 int height;
	 int channels;
 } view_cache_t;
 
 #define CACHE_CAPACITY 16
 static view_cache_t cache[CACHE_CAPACITY];
 static int cache_count = 0;
 static int cache_capacity = CACHE_CAPACITY;
 
 /* Protovtipovi unutrašnjih funkcija u ovom fajlu */
  bool viewer_img_load(viewer_t *viewer, const char *src_path, gfx_bitmap_t **rbitmap, gfx_rect_t *rect);
  bool viewer_img_setup(viewer_t *viewer, gfx_bitmap_t *bmp, gfx_rect_t *rect);
 
/**
 * 🛠️ POPRAVKA ZA LINKER U SRBIN OS (ldexp zamena za HDR format)
 * Pošto nativni libc nema ldexp funkciju, ova zamena ručno računa x * (2^exp)
 * preko brzog bitšiftinga (bit-shifting) za stepene dvojke.
 */
double ldexp(double x, int exp)
{
	/* Ako je eksponent nula, rezultat je sam broj */
	if (exp == 0) return x;
	
	/* Ako je eksponent pozitivan, množimo sa 2 preko petlje */
	if (exp > 0) {
		while (exp--) x *= 2.0;
	} 
	/* Ako je eksponent negativan, delimo sa 2 */
	else {
		while (exp++) x /= 2.0;
	}
	
	return x;
}

/* Tabela za brzu Base64 inverznu lookup pretragu */
static const char base64_inv_table[256] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -2, -1, -1,
	-1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
	-1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1
};

/**
 * Dekodira Base64 tekstualni niz u sirove bajtove direktno u RAM memoriji.
 */
static unsigned char *base64_decode(const char *data, size_t input_len, size_t *output_len)
{
	if (input_len == 0 || input_len % 4 != 0) return NULL;

	size_t out_len = (input_len / 4) * 3;
	if (data[input_len - 1] == '=') out_len--;
	if (data[input_len - 2] == '=') out_len--;

	unsigned char *out = malloc(out_len);
	if (!out) return NULL;

	size_t i, j;
	for (i = 0, j = 0; i < input_len; i += 4, j += 3) {
		char n0 = base64_inv_table[(unsigned char)data[i]];
		char n1 = base64_inv_table[(unsigned char)data[i + 1]];
		char n2 = base64_inv_table[(unsigned char)data[i + 2]];
		char n3 = base64_inv_table[(unsigned char)data[i + 3]];

		out[j] = (n0 << 2) | (n1 >> 4);
		if (n2 != -2 && j + 1 < out_len) out[j + 1] = (n1 << 4) | (n2 >> 2);
		if (n3 != -2 && j + 2 < out_len) out[j + 2] = (n2 << 6) | n3;
	}

	*output_len = out_len;
	return out;
}

/* Pomoćna funkcija za sintaksu */
static void print_syntax(void)
{
	printf("Sintaksa: %s [<opcije>] <adresa-slike>...\n", NAME);
	printf("\t-d <display-spec> koristi naznaceni displej\n");
	printf("\t-f                Mod Pun ekran\n");
}

/* =========================================================================
 * Message Types (use as first argument to msg):
 *
 * 0 = Warning
 * 1 = Note
 * 2 = Greska
 * 3 = Info
 * 4 = Success
 * 5 = Critical
 * =========================================================================
 */
static void show_message( int tip,const char *message)
{
    task_id_t id;

    // Spawn the msg app with type=1 (Note)
    errno_t rc = task_spawnl(&id, NULL,
        "/app/msg", "msg", "1", message, NULL);

    if (rc != EOK) {
        // Fallback if msg app fails
        printf("Greska: %s\n", message);
    }
}

/**
 * Prilagođena funkcija iz Pauk Browsera za Slikovnicu.
 * Uzima sirovi tekst, parsuje ga, skalira na dimenzije prozora i vraca RGB bafer.
 */
unsigned char *convert_svg_to_rgb(const char *svg_text, int w, int h)
{
	NSVGimage *image = nsvgParse((char *)svg_text, "px", 96.0f);
	if (!image) return NULL;

	/* Alokacija RGBA bafera za NanoSVG rasterizaciju (4 bajta po pikselu) */
	unsigned char *rgba = malloc(w * h * 4);
	if (!rgba) {
		nsvgDelete(image);
		return NULL;
	}

	NSVGrasterizer *rast = nsvgCreateRasterizer();
	
	/* Prilagođavamo veličinu vektora ciljanoj širini i visini bitmape */
	float scale_x = (float)w / image->width;
	float scale_y = (float)h / image->height;
	float scale = (scale_x < scale_y) ? scale_x : scale_y;

	/* Rasterizacija u sirovi RGBA bafer */
	nsvgRasterize(rast, image, 0.0f, 0.0f, scale, rgba, w, h, w * 4);

	/* Alokacija konačnog RGB niza */
	unsigned char *rgb = malloc(w * h * 3);
	if (rgb) {
		for (int i = 0; i < w * h; i++) {
			int r = rgba[i * 4];
			int g = rgba[i * 4 + 1];
			int b = rgba[i * 4 + 2];
			int a = rgba[i * 4 + 3]; /* Stvarna providnost oblika */

			/* 
			 * MATEMATIČKI BLEND SA BELOM POZADINOM (Rešava transparentnu masku OS-a):
			 * Ako je piksel providan (a saopštava stepen providnosti), stapi ga sa belom (255).
			 * Formula: nova_boja = (boja * a + 255 * (255 - a)) / 255;
			 * Odmah pakujemo u BGR raspored koji tvoja petlja u viewer_img_load očekuje!
			 */
			rgb[i * 3]     = (b * a + 255 * (255 - a)) / 255; /* B */
			rgb[i * 3 + 1] = (g * a + 255 * (255 - a)) / 255; /* G */
			rgb[i * 3 + 2] = (r * a + 255 * (255 - a)) / 255; /* R */
		}
	}

	free(rgba);
	nsvgDeleteRasterizer(rast);
	nsvgDelete(image);
	
	return rgb;
}





static void wnd_close(ui_window_t *window, void *arg)
{
	viewer_t *viewer = (viewer_t *) arg;
	ui_quit(viewer->ui);
}

static void viewer_kbd_event_unmod(viewer_t *viewer, kbd_event_t *event)
{
	if (event->key == KC_Q || event->key == KC_ESCAPE)
		ui_quit(viewer->ui);
}

static void viewer_kbd_event_ctrl(viewer_t *viewer, kbd_event_t *event)
{
	if (event->key == KC_Q)
		ui_quit(viewer->ui);
}

static void wnd_kbd_event(ui_window_t *window, void *arg, kbd_event_t *event)
{
	viewer_t *viewer = (viewer_t *) arg;

	if (event->type != KEY_PRESS)
		return;

	if ((event->mods & (KM_CTRL | KM_ALT | KM_SHIFT)) == 0)
		viewer_kbd_event_unmod(viewer, event);

	if ((event->mods & KM_CTRL) != 0 && (event->mods & (KM_ALT | KM_SHIFT)) == 0)
		viewer_kbd_event_ctrl(viewer, event);
}

/* Glavna callback struktura koja se prosleđuje ui_window_set_cb */
static ui_window_cb_t window_cb = {
	.close = wnd_close,
	.kbd = wnd_kbd_event
};

static char* make_key(const char *src_path, int target_width, int target_height) {
    char *key = malloc(512);
    snprintf(key, 512, "%s_%dx%d", src_path, target_width, target_height);
    return key;
}

 /* 
  * PRILAGOĐENA FUNKCIJA: get_cached_image
  * Radi samo preko STB-a (bez WebP, SVG i JPG/JPEG) kako si tražio.
  * Koristi str_ncmp umesto strncmp.
  */
 unsigned char *get_cached_image(const char *src_path, int target_width, int target_height,
	int *out_width, int *out_height)
{
	if (!src_path || !out_width || !out_height)
		return NULL;

	unsigned char *decoded_base64 = NULL;
	size_t base64_len = 0;
	char mime_tip[64] = {0};
	const char *ext = NULL;

	/* === AKTIVNI MEMORIJSKI ŠTIT: Presrećemo i dekodiramo inline Base64 datoteke === */
	if (str_ncmp(src_path, "data:", 5) == 0) {
		printf("🔄 [Slikovnica] Detektovan inline Data URL. Pokrecem Base64 parser...\n");
		
		const char *zarez = str_str(src_path, ",");
		if (!zarez) return NULL;

		/* Izvlačimo pod-format slike (MIME tip) radi detekcije (npr. image/png) */
		const char *mime_start = str_str(src_path, "image Weiss/");
		if (!mime_start) mime_start = str_str(src_path, "image/");
		
		if (mime_start && mime_start < zarez) {
			size_t tip_len = 0;
			const char *mime_end = mime_start;
			while (mime_end < zarez && *mime_end != ';' && *mime_end != ' ') {
				mime_end++;
			}
			tip_len = mime_end - mime_start;
			if (tip_len < sizeof(mime_tip)) {
				for (size_t k = 0; k < tip_len; k++) mime_tip[k] = mime_start[k];
				mime_tip[tip_len] = '\0';
			}
		}

		/* Proveravamo da li tekst sadrži Base64 oznaku i dekodiramo ga u RAM */
		if (str_str(src_path, ";base64") != NULL) {
			const char *kodirani_tekst = zarez + 1;
			size_t raw_txt_len = str_size(kodirani_tekst);
			
			decoded_base64 = base64_decode(kodirani_tekst, raw_txt_len, &base64_len);
			if (!decoded_base64) {
				printf("❌ [Slikovnica] Neuspesno dekodiranje Base64 stringa!\n");
				return NULL;
			}
			printf("✅ [Slikovnica] RAM Base64 dekodiran (%zu bajtova). Format: %s\n", base64_len, mime_tip);
		}
	} else {
		/* Ako je regularan fajl, ekstenziju čitamo standardno iz putanje */
		ext = str_rchr(src_path, '.');
	}

	/* Provera skaliranog keša u memoriji (Ova tvoja petlja ostaje netaknuta) */
	char *key = make_key(src_path, target_width, target_height);
	for (int i = 0; i < cache_count; i++) {
		if (str_cmp(cache[i].key, key) == 0) {
			free(key);
			if (decoded_base64) free(decoded_base64);
			*out_width = cache[i].width;
			*out_height = cache[i].height;
			return cache[i].data;
		}
	}
	free(key);

	int img_width = 0, img_height = 0, channels = 3;
	unsigned char *img_data = NULL;
	unsigned char *scaled_data = NULL;

	/* Detekcija formata - proširena i za MIME tipove iz Base64 zaglavlja */
	int is_jpeg = (ext && (str_cmp(ext, ".jpg") == 0 || str_cmp(ext, ".jpeg") == 0 ||
						   str_cmp(ext, ".JPG") == 0 || str_cmp(ext, ".JPEG") == 0)) ||
	              (str_cmp(mime_tip, "image/jpeg") == 0 || str_cmp(mime_tip, "image/jpg") == 0);

	int is_webp = (ext && (str_cmp(ext, ".webp") == 0 || str_cmp(ext, ".WEBP") == 0)) ||
	              (str_cmp(mime_tip, "image/webp") == 0);

	int is_svg  = (ext && (str_cmp(ext, ".svg") == 0  || str_cmp(ext, ".SVG") == 0)) ||
	              (str_cmp(mime_tip, "image/svg+xml") == 0);
/* NOVO: Detekcija ICO formata preko ekstenzije ili MIME tipa */
   int is_ico  = (ext && (str_cmp(ext, ".ico") == 0  || str_cmp(ext, ".ICO") == 0)) ||
                 (str_cmp(mime_tip, "image/x-icon") == 0 || str_cmp(mime_tip, "image/vnd.microsoft.icon") == 0);

	/* ===== PIPELINE 1: SVG (Pauk uvezena logika sa convert_svg_to_rgb) ===== */
	if (is_svg) {
		if (decoded_base64) {
			/* Kod Base64 SVG-a, podatak je čist tekst koji smo već uspešno izvukli u RAM */
			char *svg_text = malloc(base64_len + 1);
			if (svg_text) {
				memcpy(svg_text, decoded_base64, base64_len);
				svg_text[base64_len] = '\0';
				scaled_data = convert_svg_to_rgb(svg_text, target_width, target_height);
				if (scaled_data) {
					img_width = target_width;
					img_height = target_height;
				}
				free(svg_text);
			}
			free(decoded_base64);
			decoded_base64 = NULL;
		} else {
			/* Tvoj originalni VFS kod za ucitavanje obicnog SVG fajla sa diska */
			printf("📓 [Slikovnica] Ucitavam SVG preko VFS-a: %s\n", src_path);
			int fd;
			errno_t rc = vfs_lookup_open(src_path, WALK_REGULAR, MODE_READ, &fd);
			if (rc == EOK) {
				vfs_stat_t stat;
				rc = vfs_stat(fd, &stat);
				if (rc == EOK) {
					char *svg_text = malloc(stat.size + 1);
					size_t nread;
					if (svg_text && vfs_read(fd, (aoff64_t []) { 0 }, svg_text, stat.size, &nread) == EOK) {
						svg_text[stat.size] = '\0';
						scaled_data = convert_svg_to_rgb(svg_text, target_width, target_height);
						if (scaled_data) {
							img_width = target_width;
							img_height = target_height;
						}
					}
					if (svg_text) free(svg_text);
				}
				vfs_put(fd);
			}
		}
		
		if (!scaled_data) {
			printf("❌ [Slikovnica] convert_svg_to_rgb nije uspeo da rasterizuje.\n");
			return NULL;
		}
		goto cache_pipeline;
	}
	if (is_ico) {
		printf("📓 [Slikovnica] Pokrecem brzi ICO parser za: %s\n", src_path);
		int fd;
		errno_t rc = vfs_lookup_open(src_path, WALK_REGULAR, MODE_READ, &fd);
		if (rc == EOK) {
			vfs_stat_t stat;
			rc = vfs_stat(fd, &stat);
			if (rc == EOK) {
				unsigned char *ico_buffer = malloc(stat.size);
				size_t nread;
				if (ico_buffer && vfs_read(fd, (aoff64_t []) { 0 }, ico_buffer, stat.size, &nread) == EOK) {
					
					/* Tražimo PNG potpis (0x89, 0x50, 0x4E, 0x47) unutar fajla */
					size_t png_offset = 0;
					bool pronadjen_png = false;
					
					for (size_t i = 0; i < stat.size - 4; i++) {
						if (ico_buffer[i] == 0x89 && ico_buffer[i+1] == 0x50 && 
						    ico_buffer[i+2] == 0x4E && ico_buffer[i+3] == 0x47) {
							png_offset = i;
							pronadjen_png = true;
							break;
						}
					}
					
					if (pronadjen_png) {
						printf("🎯 [Slikovnica] Pronadjena PNG struktura unutar ikone na pomeraju: %zu\n", png_offset);
						
						/* Prosleđujemo ugrađeni PNG segment pravo našem STB-u u RAM-u! */
						size_t png_velicina = stat.size - png_offset;
						img_data = stbi_load_from_memory(&ico_buffer[png_offset], png_velicina, &img_width, &img_height, &channels, 3);
					} else {
						/* ===== NAPREDNI FALLBACK: Parser za klasične Microsoft BMP ikone ===== */
						printf("📓 [Slikovnica] Ikona koristi stari BMP kontejner. Čitam ICO zaglavlje...\n");
						
						/* Provera broja slika unutar ikone (bajtovi 4-5 u zaglavlju) */
						uint16_t broj_slika = ico_buffer[4] | (ico_buffer[5] << 8);
						
						if (broj_slika > 0 && stat.size > 22) {
							/* 
							 * Čitamo podatke za prvu sliku iz direktorijuma ikone:
							 * bajtovi 14-17: veličina BMP podataka unutar fajla
							 * bajtovi 18-21: tačan početni pomeraj (offset) gde počinje BMP
							 */
							uint32_t bmp_velicina = ico_buffer[14] | (ico_buffer[15] << 8) | 
							                        (ico_buffer[16] << 16) | (ico_buffer[17] << 24);
							uint32_t bmp_offset   = ico_buffer[18] | (ico_buffer[19] << 8) | 
							                        (ico_buffer[20] << 16) | (ico_buffer[21] << 24);
							
							/* Bezbednosna provera da ne izletimo iz alociranog bafera */
							if (bmp_offset + bmp_velicina <= stat.size) {
								printf("🎯 [Slikovnica] Lociran ugradjen BMP: pomeraj=%u, velicina=%u\n", bmp_offset, bmp_velicina);
								
								/* TRANSPLANTACIJA ZAGLAVLJA: Kreiramo standardni 14-bajtni BITMAPFILEHEADER u RAM-u */
								size_t puni_bmp_size = 14 + bmp_velicina;
								unsigned char *cist_bmp = malloc(puni_bmp_size);
								
								if (cist_bmp) {
									cist_bmp[0] = 'B';
									cist_bmp[1] = 'M';
									
									uint32_t f_size = (uint32_t)puni_bmp_size;
									cist_bmp[2] = f_size & 0xff;
									cist_bmp[3] = (f_size >> 8) & 0xff;
									cist_bmp[4] = (f_size >> 16) & 0xff;
									cist_bmp[5] = (f_size >> 24) & 0xff;
									
									cist_bmp[6] = 0; cist_bmp[7] = 0;
									cist_bmp[8] = 0; cist_bmp[9] = 0;
									
									uint32_t pixels_offset = 54;
									cist_bmp[10] = pixels_offset & 0xff;
									cist_bmp[11] = (pixels_offset >> 8) & 0xff;
									cist_bmp[12] = (pixels_offset >> 16) & 0xff;
									cist_bmp[13] = (pixels_offset >> 24) & 0xff;
									
									/* Nadovezujemo ostatak slike iz ikone */
									for (size_t k = 0; k < bmp_velicina; k++) {
										cist_bmp[14 + k] = ico_buffer[bmp_offset + k];
									}
									
									printf("🛠️ [Slikovnica] Rekonstruisan cist BMP u RAM-u. Saljem STB-u...\n");
									img_data = stbi_load_from_memory(cist_bmp, puni_bmp_size, &img_width, &img_height, &channels, 3);
									free(cist_bmp);
								}
							}
						}
						
						/* Ako gornji proračun propadne, probaj fallback na celi fajl kao poslednju nadu */
						if (!img_data) {
							printf("⏭️ [Slikovnica] Pokusavam direktan STB load celog fajla...\n");
							img_data = stbi_load_from_memory(ico_buffer, stat.size, &img_width, &img_height, &channels, 3);
						}
					}
				}
				if (ico_buffer) free(ico_buffer);
			}
			vfs_put(fd);
		}
		
		if (!img_data) {
			printf("❌ [Slikovnica] Neuspesno dekodiranje .ico fajla.\n");
			return NULL;
		}
	}


	/* ===== PIPELINE 2: Učitavanje WebP-a preko libwebp (Čist RAM sloj) ===== */
	else if (is_webp) {
		if (decoded_base64) {
			img_data = WebPDecodeRGB(decoded_base64, base64_len, &img_width, &img_height);
			if (img_data) channels = 3;
			free(decoded_base64);
			decoded_base64 = NULL;
		} else {
			/* Tvoj originalni VFS kod za ucitavanje obicnog WebP fajla */
			printf("📓 [Slikovnica] libwebp ucitava: %s\n", src_path);
			int fd;
			errno_t rc = vfs_lookup_open(src_path, WALK_REGULAR, MODE_READ, &fd);
			if (rc == EOK) {
				vfs_stat_t stat;
				rc = vfs_stat(fd, &stat);
				if (rc == EOK) {
					unsigned char *webp_buffer = malloc(stat.size);
					size_t nread;
					if (webp_buffer && vfs_read(fd, (aoff64_t []) { 0 }, webp_buffer, stat.size, &nread) == EOK) {
						img_data = WebPDecodeRGB(webp_buffer, stat.size, &img_width, &img_height);
						if (img_data) channels = 3;
					}
					if (webp_buffer) free(webp_buffer);
				}
				vfs_put(fd);
			}
		}
		if (!img_data) return NULL;
	}
	/* ===== PIPELINE 3: Učitavanje JPEG-a preko libjpeg (Čist RAM sloj) ===== */
	else if (is_jpeg) {
		struct jpeg_decompress_struct cinfo;
		struct jpeg_error_mgr jerr;
		cinfo.err = jpeg_std_error(&jerr);
		jpeg_create_decompress(&cinfo);

		if (decoded_base64) {
			jpeg_mem_src(&cinfo, decoded_base64, base64_len);
			if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
				cinfo.out_color_space = JCS_RGB;
				jpeg_start_decompress(&cinfo);
				img_width = cinfo.output_width;
				img_height = cinfo.output_height;
				unsigned long row_stride = cinfo.output_width * cinfo.output_components;
				img_data = malloc(row_stride * cinfo.output_height);
				if (img_data) {
					JSAMPROW row_pointer[1];
					while (cinfo.output_scanline < cinfo.output_height) {
						row_pointer[0] = &img_data[cinfo.output_scanline * row_stride];
						jpeg_read_scanlines(&cinfo, row_pointer, 1);
					}
				}
				jpeg_finish_decompress(&cinfo);
			}
			jpeg_destroy_decompress(&cinfo);
			free(decoded_base64);
			decoded_base64 = NULL;
		} else {
			/* Tvoj originalni VFS kod za ucitavanje obicnog JPEG fajla */
			printf("📓 [Slikovnica] libjpeg ucitava: %s\n", src_path);
			int fd;
			errno_t rc = vfs_lookup_open(src_path, WALK_REGULAR, MODE_READ, &fd);
			if (rc == EOK) {
				vfs_stat_t stat;
				rc = vfs_stat(fd, &stat);
				if (rc == EOK) {
					unsigned char *jpeg_buffer = malloc(stat.size);
					size_t nread;
					if (jpeg_buffer && vfs_read(fd, (aoff64_t []) { 0 }, jpeg_buffer, stat.size, &nread) == EOK) {
						jpeg_mem_src(&cinfo, jpeg_buffer, stat.size);
						if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
							cinfo.out_color_space = JCS_RGB;
							jpeg_start_decompress(&cinfo);
							img_width = cinfo.output_width;
							img_height = cinfo.output_height;
							unsigned long row_stride = cinfo.output_width * cinfo.output_components;
							img_data = malloc(row_stride * cinfo.output_height);
							if (img_data) {
								JSAMPROW row_pointer[1];
								while (cinfo.output_scanline < cinfo.output_height) {
									row_pointer[0] = &img_data[cinfo.output_scanline * row_stride];
									jpeg_read_scanlines(&cinfo, row_pointer, 1);
								}
							}
							jpeg_finish_decompress(&cinfo);
						}
						jpeg_destroy_decompress(&cinfo);
					}
					if (jpeg_buffer) free(jpeg_buffer);
				}
				vfs_put(fd);
			}
		}
		if (!img_data) return NULL;
	}
	/* ===== POSTOJEĆI PIPELINE: Ostali formati preko STB (Ubačen RAM load) ===== */
	else {
		if (decoded_base64) {
			/* STB ucitava sirove dekodirane bajtove direktno iz RAM-a! */
			printf("📓 [Slikovnica] STB ucitava Base64 bajtove iz memorije...\n");
			img_data = stbi_load_from_memory(decoded_base64, base64_len, &img_width, &img_height, &channels, 3);
			free(decoded_base64);
			decoded_base64 = NULL;
		} else {
			/* Tvoj originalni kod za učitavanje sa diska (Uključujući i HDR BGR popravku) */
			printf("📓 [Slikovnica] STB ucitava format: %s\n", src_path);
			img_data = stbi_load(src_path, &img_width, &img_height, &channels, 3);
			
			if (img_data) {
				int is_hdr_file = (ext && (str_cmp(ext, ".hdr") == 0 || str_cmp(ext, ".HDR") == 0));
				if (is_hdr_file) {
					for (int i = 0; i < img_width * img_height; i++) {
						unsigned char privremena_r = img_data[i * 3];
						img_data[i * 3]     = img_data[i * 3 + 2];
						img_data[i * 3 + 2] = privremena_r;
					}
					printf("🎨 [Slikovnica] Izvrsena uspesna BGR korekcija za HDR piksele.\n");
				}
			}
		}
		
		if (!img_data) {
			printf("❌ [Slikovnica] stb_image ne moze da ucita fajl: %s\n", stbi_failure_reason());
			return NULL;
		}
		printf("✅ [Slikovnica] stb_image uspesno ucitao sliku: %dx%d\n", img_width, img_height);
	}

	/* Tvoja petlja za Nearest Neighbor skaliranje i upis u keš ostaju potpuno isti... */
	scaled_data = (unsigned char *) malloc(target_width * target_height * 3);
	if (!scaled_data) {
		if (is_jpeg) free(img_data);
		else stbi_image_free(img_data);
		return NULL;
	}

	for (int y = 0; y < target_height; y++) {
		for (int x = 0; x < target_width; x++) {
			int src_x = (x * img_width) / target_width;
			int src_y = (y * img_height) / target_height;
			
			if (src_x >= img_width) src_x = img_width - 1;
			if (src_y >= img_height) src_y = img_height - 1;

			int src_idx = (src_y * img_width + src_x) * 3;
			int dst_idx = (y * target_width + x) * 3;

			scaled_data[dst_idx] = img_data[src_idx];
			scaled_data[dst_idx + 1] = img_data[src_idx + 1];
			scaled_data[dst_idx + 2] = img_data[src_idx + 2];
		}
	}

	if (is_webp) {
		/* libwebp zahteva WebPFree za podatke koje je sama alocirala */
		WebPFree(img_data);
	} else if (is_jpeg) {
		free(img_data);
	} else {
		stbi_image_free(img_data);
	}

cache_pipeline:
	/* Keširanje skalirane slike (Tvoj originalni završetak funkcije) */
	{
		char *new_key = make_key(src_path, target_width, target_height);
		
		if (cache_count >= cache_capacity) {
			free(cache[0].key);
			free(cache[0].data);
			for (int i = 1; i < cache_count; i++) {
				cache[i - 1] = cache[i];
			}
			cache_count--;
		}

		cache[cache_count].key = new_key;
		cache[cache_count].data = scaled_data;
		cache[cache_count].width = target_width;
		cache[cache_count].height = target_height;
		cache[cache_count].channels = 3;
		cache_count++;

		*out_width = target_width;
		*out_height = target_height;
	}

	return scaled_data;
}




 
/** Pomoc / Procitaj stavka iz menija izabrana.
 * Ucitava podrazumevanu sliku direktno kroz memorijski kes.
 *
 * @param mentry Stavka menija
 * @param arg Argument (viewer_t *)
 */
void edit_file_procitaj(ui_menu_entry_t *mentry, void *arg)
{
	(void)mentry;
	  const char *help_file = "data/pomoc/slikovnica.html";
	  task_id_t tid;
	  int rc;
  
	  rc = task_spawnl(&tid, NULL, HELP_APP, HELP_APP, help_file, NULL);
	  if (rc != EOK) {
		  printf("Greska: Neuspelo pokretanje pomoci u (%s)\n", HELP_APP);
	  
	  }
}

/** Prikazi tekst na statusnoj liniji. 
 *
 * @param str Tekstualna poruka za prikaz
 * @param arg Pokazivač na ui_label_t kontrolu status bara
 */
void status_display(char const *str, void *arg)
{
	ui_label_t *status = (ui_label_t *) arg;
	
	if (status != NULL && str != NULL) {
		(void) ui_label_set_text(status, str);
		(void) ui_label_paint(status);
	}
}

/** Uredi / Rotiraj stavka iz menija izabrana.
 * Rotira sliku u RAM-u čišćenjem keša i prisilnim ponovnim učitavanjem piksela.
 *
 * @param mentry Stavka menija
 * @param arg Argument (viewer_t *)
 */
void edit_file_rotiraj(ui_menu_entry_t *mentry, void *arg)
{
	viewer_t *viewer = (viewer_t *) arg;
	gfx_bitmap_t *lbitmap = NULL;
	gfx_rect_t lrect;

	(void) mentry;

	if (viewer == NULL || viewer->fname == NULL || viewer->imgs == NULL) {
		status_display("Ucitaj Sliku koju bi da ROTIRAS", viewer->status);
		return;
	}

	/* Povećavamo ugao za po 90 stepeni u krug (0 -> 90 -> 180 -> 270 -> 0) */
	viewer->ugao_rotacije = (viewer->ugao_rotacije + 90) % 360;
	printf("[Slikovnica] Rotiram na ugao: %d stepeni unutar RAM-a\n", viewer->ugao_rotacije);

	/* Trenutni reload iz keša sa novom matricom ugla */
	if (!viewer_img_load(viewer, viewer->imgs[0], &lbitmap, &lrect)) {
		printf("❌ [Slikovnica] Rotacija u memoriji nije uspela.\n");
		return;
	}

	/* Forsirano osvežavanje prikaza na ekranu */
	if (viewer->image != NULL) {
		(void) ui_image_paint(viewer->image);
	}
	(void) ui_fixed_paint(viewer->fixed);
	(void) ui_window_paint(viewer->window);
}




 /* 
	 KONAČNA IMPLEMENTACIJA: viewer_img_load
	 Crta direktno na nativnu HelenOS/SrBinOS bitmapu, bez pixelmape i bez diska.
 */

/* =========================================================================
 * IMPLEMENTACIJA: viewer_img_load
 * =========================================================================
 */
bool viewer_img_load(viewer_t *viewer, const char *src_path,
    gfx_bitmap_t **rbitmap, gfx_rect_t *rect)
{
    if (!src_path || !rbitmap || !rect)
        return false;

    ui_wnd_params_t params;
    gfx_rect_t wrect;
    gfx_coord2_t off;
    gfx_bitmap_alloc_t alloc;
    gfx_bitmap_t *new_bmp = NULL;
    gfx_bitmap_params_t bmp_params;
    errno_t rc;

    gfx_coord_t max_width = 500 - 10;
    gfx_coord_t max_height = 500 - 60;

    int orig_w = 0;
    int orig_h = 0;
    int orig_comp = 0;

    /* Provera da li je inline Base64 podatak umesto fajla sa diska. */
    int is_base64 = (str_ncmp(src_path, "data:", 5) == 0);
    int is_webp = 0;
    int is_svg = 0;
    int is_jpeg = 0;
    int is_hdr = 0;
    int is_ico = 0; /* 1. Deklaracija zastavice na samom vrhu funkcije */

    const char *ext = NULL; /* 2. Inicijalizacija pokazivaca na NULL */

    /* ===== NOVI GLOBALNI ŠTIT ZA INLINE BASE64 SLIKE ===== */
    if (is_base64) {
        orig_w = max_width;
        orig_h = max_height;
        orig_comp = 3;

        printf("🔄 [Slikovnica] viewer_img_load propusta inline Base64 bez ekstenzije...\n");
    }
    /* ===== AKO JE OBIČAN FAJL, RADI STANDARDNE PROVERE EKSTENZIJA ===== */
    else {
        ext = str_rchr(src_path, '.');

        /* 3. Sada je ext procitan i bezbedno ga koristimo za sve provere unutar else bloka */
        is_ico = (ext &&
            (str_cmp(ext, ".ico") == 0 ||
             str_cmp(ext, ".ICO") == 0));

        is_webp = (ext &&
            (str_cmp(ext, ".webp") == 0 ||
             str_cmp(ext, ".WEBP") == 0));

        is_svg = (ext &&
            (str_cmp(ext, ".svg") == 0 ||
             str_cmp(ext, ".SVG") == 0));

        is_hdr = (ext &&
            (str_cmp(ext, ".hdr") == 0 ||
             str_cmp(ext, ".HDR") == 0));

        is_jpeg = (ext &&
            (str_cmp(ext, ".jpg") == 0 ||
             str_cmp(ext, ".jpeg") == 0 ||
             str_cmp(ext, ".JPG") == 0 ||
             str_cmp(ext, ".JPEG") == 0));

        if (is_webp) {
            int fd;

            rc = vfs_lookup_open(src_path, WALK_REGULAR, MODE_READ, &fd);

            if (rc == EOK) {
                vfs_stat_t stat;

                if (vfs_stat(fd, &stat) == EOK) {
                    unsigned char *webp_header =
                        malloc(stat.size < 32 ? stat.size : 32);

                    size_t nread;

                    if (webp_header &&
                        vfs_read(fd, (aoff64_t []) { 0 },
                            webp_header,
                            stat.size < 32 ? stat.size : 32,
                            &nread) == EOK) {

                        if (WebPGetInfo(webp_header, nread,
                            &orig_w, &orig_h)) {
                            orig_comp = 3;
                        }
                    }

                    if (webp_header)
                        free(webp_header);
                }

                vfs_put(fd);
            }

            if (orig_w <= 0 || orig_h <= 0) {
                printf("❌ [Slikovnica] libwebp nije uspeo da procita zaglavlje WebP fajla.\n");
                return false;
            }
        }
        else if (is_svg) {
            orig_w = max_height;
            orig_h = max_height;
            orig_comp = 3;
        }
		else if (is_ico) {
            orig_w = max_width;  /* 490 */
            orig_h = max_height; /* 440 */
            orig_comp = 3;
        }
        else if (is_jpeg) {
            int fd;

            rc = vfs_lookup_open(src_path, WALK_REGULAR, MODE_READ, &fd);

            if (rc == EOK) {
                vfs_stat_t stat;

                if (vfs_stat(fd, &stat) == EOK) {
                    unsigned char *jpeg_buffer = malloc(stat.size);
                    size_t nread;

                    if (jpeg_buffer &&
                        vfs_read(fd, (aoff64_t []) { 0 },
                            jpeg_buffer, stat.size, &nread) == EOK) {

                        struct jpeg_decompress_struct cinfo;
                        struct jpeg_error_mgr jerr;

                        cinfo.err = jpeg_std_error(&jerr);

                        jpeg_create_decompress(&cinfo);
                        jpeg_mem_src(&cinfo, jpeg_buffer, stat.size);

                        if (jpeg_read_header(&cinfo, TRUE) ==
                            JPEG_HEADER_OK) {
                            orig_w = cinfo.image_width;
                            orig_h = cinfo.image_height;
                            orig_comp = 3;
                        }

                        jpeg_destroy_decompress(&cinfo);
                    }

                    if (jpeg_buffer)
                        free(jpeg_buffer);
                }

                vfs_put(fd);
            }

            if (orig_w <= 0 || orig_h <= 0) {
                printf("❌ [Slikovnica] libjpeg nije uspeo da procita zaglavlje JPEG fajla.\n");
                return false;
            }
        }
        else if (is_hdr) {
            int fd;

            rc = vfs_lookup_open(src_path, WALK_REGULAR, MODE_READ, &fd);

            if (rc == EOK) {
                vfs_stat_t stat;

                if (vfs_stat(fd, &stat) == EOK) {
                    size_t header_size =
                        stat.size < 128 ? stat.size : 128;

                    unsigned char *hdr_header = malloc(header_size);
                    size_t nread;

                    if (hdr_header &&
                        vfs_read(fd, (aoff64_t []) { 0 },
                            hdr_header, header_size, &nread) == EOK) {

                        if (stbi_info_from_memory(
                            hdr_header, nread,
                            &orig_w, &orig_h, &orig_comp)) {

                            printf("✅ [Slikovnica] STB uspesno procitao HDR strukturu iz RAM-a: %dx%d\n",
                                orig_w, orig_h);
                        }
                    }

                    if (hdr_header)
                        free(hdr_header);
                }

                vfs_put(fd);
            }

            if (orig_w <= 0 || orig_h <= 0) {
                printf("❌ [Slikovnica] STB nije uspeo da procita HDR zaglavlje.\n");
                return false;
            }
        }
        else {
            if (!stbi_info(src_path, &orig_w, &orig_h, &orig_comp)) {
                printf("❌ [Slikovnica] stbi_info nije uspeo da procita zaglavlje fajla.\n");
                return false;
            }
        }
    }

    int efektivni_w = orig_w;
    int efektivni_h = orig_h;

    if (viewer->ugao_rotacije == 90 ||
        viewer->ugao_rotacije == 270) {
        efektivni_w = orig_h;
        efektivni_h = orig_w;
    }

    double faktor_w = (double) max_width / efektivni_w;
    double faktor_h = (double) max_height / efektivni_h;

    double faktor = (faktor_w < faktor_h) ? faktor_w : faktor_h;

    if (faktor > 1.0)
        faktor = 1.0;

    gfx_coord_t width = efektivni_w * faktor;
    gfx_coord_t height = efektivni_h * faktor;

    rect->p0.x = 5 + (max_width - width) / 2;
    rect->p1.x = rect->p0.x + width;

    rect->p0.y = 25 + (max_height - height) / 2;
    rect->p1.y = rect->p0.y + height;

    int target_cache_w =
        (viewer->ugao_rotacije == 90 ||
         viewer->ugao_rotacije == 270) ? height : width;

    int target_cache_h =
        (viewer->ugao_rotacije == 90 ||
         viewer->ugao_rotacije == 270) ? width : height;

    int img_width = 0;
    int img_height = 0;

    unsigned char *img_data = get_cached_image(
        src_path,
        target_cache_w,
        target_cache_h,
        &img_width,
        &img_height);

    if (!img_data || img_width <= 0 || img_height <= 0) {
        printf("❌ [Slikovnica] get_cached_image nije vratio piksele.\n");
        return false;
    }

    gfx_bitmap_params_init(&bmp_params);
    bmp_params.rect = *rect;

    rc = gfx_bitmap_create(
        viewer->window_gc, &bmp_params, NULL, &new_bmp);

    if (rc != EOK || new_bmp == NULL)
        return false;

    rc = gfx_bitmap_get_alloc(new_bmp, &alloc);

    if (rc != EOK) {
        gfx_bitmap_destroy(new_bmp);
        return false;
    }

    uint32_t *dst_pixels = (uint32_t *) alloc.pixels;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int src_x = 0;
            int src_y = 0;

            if (viewer->ugao_rotacije == 0) {
                src_x = (x * img_width) / width;
                src_y = (y * img_height) / height;
            }
            else if (viewer->ugao_rotacije == 90) {
                src_x = (y * img_width) / height;
                src_y = ((width - 1 - x) * img_height) / width;
            }
            else if (viewer->ugao_rotacije == 180) {
                src_x = ((width - 1 - x) * img_width) / width;
                src_y = ((height - 1 - y) * img_height) / height;
            }
            else if (viewer->ugao_rotacije == 270) {
                src_x = ((height - 1 - y) * img_width) / height;
                src_y = (x * img_height) / width;
            }

            if (src_x >= img_width)
                src_x = img_width - 1;

            if (src_y >= img_height)
                src_y = img_height - 1;

            if (src_x < 0)
                src_x = 0;

            if (src_y < 0)
                src_y = 0;

            int idx = (src_y * img_width + src_x) * 3;

            uint8_t b = img_data[idx + 2];
            uint8_t g = img_data[idx + 1];
            uint8_t r = img_data[idx];

            dst_pixels[y * width + x] =
                (r << 16) | (g << 8) | b;
        }
    }

    *rbitmap = new_bmp;
    params = viewer->params;

    ui_wdecor_rect_from_app(
        viewer->ui, params.style, rect, &wrect);

    off = wrect.p0;
    gfx_rect_rtranslate(&off, &wrect, rect);

    if (!viewer_img_setup(viewer, *rbitmap, rect)) {
        gfx_bitmap_destroy(new_bmp);
        return false;
    }

    if (viewer->image != NULL)
        (void) ui_image_paint(viewer->image);

    rc = ui_window_paint(viewer->window);
    ui_fixed_paint(viewer->fixed);

    return true;
}






 
 bool viewer_img_setup(viewer_t *viewer, gfx_bitmap_t *bmp, gfx_rect_t *rect)
 {
	 gfx_rect_t arect;
	 gfx_rect_t irect;
	 ui_resource_t *ui_res;
	 errno_t rc;
 
	 ui_res = ui_window_get_res(viewer->window);
	 ui_window_get_app_rect(viewer->window, &arect);
 
	 gfx_rect_ctr_on_rect(rect, &arect, &irect);
 
	 if (viewer->image != NULL) {
		 ui_image_set_bmp(viewer->image, bmp, rect);
		 ui_image_set_rect(viewer->image, &irect);
		 ui_image_ctl(viewer->image);
	 } else {
		 rc = ui_image_create(ui_res, bmp, rect, &viewer->image);
		 if (rc != EOK) {
			 gfx_bitmap_destroy(bmp);
			 return false;
		 }
		 rc = ui_fixed_add(viewer->fixed, ui_image_ctl(viewer->image));
		 if (rc != EOK) {
			 return false;
		 }
		 ui_image_set_rect(viewer->image, &irect);
	 }
 
	 if (viewer->bitmap != NULL) {
		 gfx_bitmap_destroy(viewer->bitmap);
	 }
 
	 viewer->bitmap = bmp;
	 ui_label_set_text(viewer->status, viewer->fname);
	 ui_label_paint(viewer->status);
 
	 return true;
 }
 

/* 
 * Uništenje prozora i čišćenje resursa
 */
void viewer_window_destroy(viewer_t *viewer)
{
	if (viewer->window != NULL)
		ui_window_destroy(viewer->window);
	viewer->window = NULL;
}

/** Slika / Izadji stavka iz menija izabrana.
 * Gasi aplikaciju i bezbedno zatvara graficki UI sistem.
 *
 * @param mentry Stavka menija
 * @param arg Argument (viewer_t *)
 */
void edit_file_exit(ui_menu_entry_t *mentry, void *arg)
{
	viewer_t *viewer = (viewer_t *) arg;
	
	(void) mentry;
	
	if (viewer != NULL && viewer->ui != NULL) {
		ui_quit(viewer->ui);
	}
}

/** Slika / Otvori stavka iz menija izabrana.
 * Pokreće sistemski File Dialog za izbor nove slike.
 *
 * @param mentry Stavka menija
 * @param arg Argument (viewer_t *)
 */
void edit_file_otvori(ui_menu_entry_t *mentry, void *arg)
{
	viewer_t *viewer = (viewer_t *) arg;
	ui_file_dialog_params_t fdparams;
	errno_t rc;

	(void) mentry;

	if (viewer == NULL || viewer->ui == NULL)
		return;

	/* Inicijalizacija parametara za otvaranje dijaloga fajlova */
	ui_file_dialog_params_init(&fdparams);
	fdparams.caption = "Ucitaj sliku";

	/* Kreiranje dijaloga u sklopu glavnog UI-ja */
	rc = ui_file_dialog_create(viewer->ui, &fdparams, &viewer->dialog);
	if (rc != EOK) {
		printf("❌ [Slikovnica] Ne mogu da kreiram dijalog za izbor fajla.\n");
		return;
	}

	/* 
	 * Povezivanje dijaloga sa callback strukturom otvori_dialog_cb.
	 * Pošto je otvori_dialog_cb deklarisana u drugom fajlu, 
	 * linker će je uspešno spojiti tokom build-a.
	 */
	extern ui_file_dialog_cb_t otvori_dialog_cb;
	ui_file_dialog_set_cb(viewer->dialog, &otvori_dialog_cb, (void *) viewer);
}

/* =========================================================================
 * 1. IMPLEMENTACIJA HANDLERA ZA FILE DIJALOG (Javne funkcije za povezivanje)
 * ========================================================================= */

/** Korisnik je izabrao sliku i pritisnuo OK */
void file_dialog_bok(ui_file_dialog_t *dialog, void *arg, const char *fname)
{
	viewer_t *viewer = (viewer_t *) arg;
	const char *stara = fname;
	gfx_bitmap_t *lbitmap = NULL;
	gfx_rect_t lrect;

	/* 1. Odmah uzmi ekstenziju iz originalnog imena fajla */
	const char *ext = daj_extenziju(stara);
	printf("Ekstenzija detektovana: %s\n", ext);

	/* 2. Provera formata - ako ekstenzija ne postoji, odbaci odmah */
	if (ext == NULL || str_cmp(ext, "") == 0) {
		ui_label_set_text(viewer->status, "Greska: Fajl nema ekstenziju!");
		ui_label_paint(viewer->status);
		ui_file_dialog_destroy(dialog);
		viewer->dialog = NULL;
		return;
	}

	/* Robusna provera pojedinačnih formata bez oslanjanja na str_casecmp */
	bool podrzano = false;
	if (str_cmp(ext, ".png") == 0 || str_cmp(ext, ".PNG") == 0 ||
	    str_cmp(ext, ".bmp") == 0 || str_cmp(ext, ".BMP") == 0 ||
	    str_cmp(ext, ".tga") == 0 || str_cmp(ext, ".TGA") == 0 ||
	    str_cmp(ext, ".gif") == 0 || str_cmp(ext, ".GIF") == 0 ||
		str_cmp(ext, ".ico") == 0 || str_cmp(ext, ".ICO") == 0 ||
		str_cmp(ext, ".pnm") == 0 || str_cmp(ext, ".PNM") == 0 ||
		str_cmp(ext, ".ppm") == 0 || str_cmp(ext, ".PPM") == 0 ||
	    str_cmp(ext, ".jpg") == 0 || str_cmp(ext, ".JPG") == 0 ||str_cmp(ext, ".Jpg") == 0 ||
	    str_cmp(ext, ".jpeg") == 0 || str_cmp(ext, ".JPEG") == 0 ||str_cmp(ext, ".Jpeg") == 0 ||
	    str_cmp(ext, ".webp") == 0 || str_cmp(ext, ".WEBP") == 0 ||
		str_cmp(ext, ".hdr") == 0 || str_cmp(ext, ".HDR") == 0 ||
	    str_cmp(ext, ".svg") == 0 || str_cmp(ext, ".SVG") == 0) {
		podrzano = true;
	}

	if (!podrzano) {
		ui_label_set_text(viewer->status, "Odabrani Format slike, NIJE podrzan !");
		show_message(1, "Odabrani format slike nije podrzan");
		ui_label_paint(viewer->status);
		ui_file_dialog_destroy(dialog);
		viewer->dialog = NULL;
		return;
	}

	printf("****** PODRZANO ******* %s\n", ext);

	/* 3. Inicijalizacija i alokacije niza slika */
	viewer->imgs_count = 1;
	viewer->imgs = calloc(viewer->imgs_count, sizeof(char *));
	if (viewer->imgs == NULL) {
		printf("Van memorije.\n");
		goto error;
	}

	viewer->imgs[0] = str_dup(stara);
	if (viewer->imgs[0] == NULL) {
		printf("Van memorije.\n");
		goto error;
	}

	/* Čuvamo čistu putanju u strukturi za prikaz na statusnoj liniji */
	viewer->fname = str_dup(stara); 

	/* 4. Pokretanje brzog memorijskog pajplajna za učitavanje */
	if (!viewer_img_load(viewer, viewer->imgs[0], &lbitmap, &lrect)) {
		printf("Sliku nije ucitao img_load iz DIJALOGA greska: %s\n", stara);
		goto error;
	}

	ui_file_dialog_destroy(dialog);
	viewer->dialog = NULL;
	return;

error:
	ui_file_dialog_destroy(dialog);
	viewer->dialog = NULL;
}

/** Korisnik je pritisnuo dugme Cancel u dijalogu */
void file_dialog_bcancel(ui_file_dialog_t *dialog, void *arg)
{
	viewer_t *viewer = (viewer_t *) arg;
	ui_file_dialog_destroy(dialog);
	viewer->dialog = NULL;
}

/** Korisnik je ugasio dijalog na taster 'X' */
void file_dialog_close(ui_file_dialog_t *dialog, void *arg)
{
	viewer_t *viewer = (viewer_t *) arg;
	ui_file_dialog_destroy(dialog);
	viewer->dialog = NULL;
}

/* =========================================================================
 * 2. ZAJEDNIČKA STURKTURA ZA EXPORT (Izbačena reč 'static')
 * ========================================================================= */
ui_file_dialog_cb_t otvori_dialog_cb = {
	.bok = file_dialog_bok,
	.bcancel = file_dialog_bcancel,
	.close = file_dialog_close
};


/* 
 * Kreiranje glavnog prozora aplikacije sa menijem i status barom
 */
errno_t viewer_window_create(viewer_t *viewer)
{
	ui_wnd_params_t params;
	ui_menu_t *mfile = NULL;
	ui_menu_entry_t *mexit = NULL;
	ui_menu_entry_t *motvori = NULL;
	ui_menu_t *muredi = NULL;
	ui_menu_entry_t *mrotiraj = NULL;
	ui_menu_t *mpomoc = NULL;
	ui_menu_entry_t *mprocitaj = NULL;
	gfx_rect_t arect;
	gfx_rect_t rect;
	errno_t rc;

	/* Inicijalizacija parametara prozora */
	ui_wnd_params_init(&params);
	params.caption = "SLIKOVNICA";
	params.style |= ui_wds_maximize_btn | ui_wds_resizable;
	params.rect.p0.x = 0;
	params.rect.p0.y = 0;
	params.rect.p1.x = 500;
	params.rect.p1.y = 500;

	if (viewer->fullscreen) {
		params.style &= ~ui_wds_decorated;
		params.placement = ui_wnd_place_full_screen;
	}
	viewer->params = params;
	viewer->img_rect = rect;

	/* Kreiranje prozora */
	rc = ui_window_create(viewer->ui, &params, &viewer->window);
	if (rc != EOK) {
		printf("Greska pri kreiranju prozora.\n");
		goto error;
	}

	viewer->window_gc = ui_window_get_gc(viewer->window);
	ui_window_set_cb(viewer->window, &window_cb, (void *) viewer);
	viewer->ui_res = ui_window_get_res(viewer->window);

	/* Kreiranje fiksnog rasporeda (Fixed Layout) */
	rc = ui_fixed_create(&viewer->fixed);
	if (rc != EOK) {
		printf("Greska pri kreiranju fiksnog izgleda.\n");
		return rc;
	}

	/* Kreiranje glavne linije menija (Menu Bar) */
	rc = ui_menu_bar_create(viewer->ui, viewer->window, &viewer->menubar);
	if (rc != EOK) {
		printf("Greska pri kreiranju meni bara.\n");
		return rc;
	}

	/* Pozicioniranje menija na vrh prozora */
	ui_window_get_app_rect(viewer->window, &arect);
	rect.p0.x = arect.p0.x;
	rect.p1.x = arect.p1.x - 10;
	rect.p0.y = arect.p0.y + 10;
	rect.p1.y = arect.p0.y + 30;
	ui_menu_bar_set_rect(viewer->menubar, &rect);

	/* --- KREIRANJE MENIJA "Slika" --- */
	rc = ui_menu_dd_create(viewer->menubar, "~S~lika", NULL, &mfile);
	if (rc != EOK) {
		printf("Greska pri kreiranju menija Slika.\n");
		return rc;
	}
	rc = ui_menu_entry_create(mfile, "~O~tvori", "Ctrl-O", &motvori);
	if (rc != EOK) {
		printf("Greska pri kreiranju stavke Otvori.\n");
		return rc;
	}
	ui_menu_entry_set_cb(motvori, edit_file_otvori, (void *) viewer);

	rc = ui_menu_entry_create(mfile, "I~z~adji", "Ctrl-Q", &mexit);
	if (rc != EOK) {
		printf("Greska pri kreiranju stavke Izadji.\n");
		return rc;
	}
	ui_menu_entry_set_cb(mexit, edit_file_exit, (void *) viewer);

	/* --- KREIRANJE MENIJA "Uredi" --- */
	rc = ui_menu_dd_create(viewer->menubar, "~U~redi", NULL, &muredi);
	if (rc != EOK) {
		printf("Greska pri kreiranju menija Uredi.\n");
		return rc;
	}
	rc = ui_menu_entry_create(muredi, "~R~otiraj", "Ctrl-R", &mrotiraj);
	if (rc != EOK) {
		printf("Greska pri kreiranju stavke Rotiraj.\n");
		return rc;
	}
	ui_menu_entry_set_cb(mrotiraj, edit_file_rotiraj, (void *) viewer);

	/* --- KREIRANJE MENIJA "Pomoc" --- */
	rc = ui_menu_dd_create(viewer->menubar, "~P~omoc", NULL, &mpomoc);
	if (rc != EOK) {
		printf("Greska pri kreiranju menija Pomoc.\n");
		return rc;
	}
	rc = ui_menu_entry_create(mpomoc, "~P~rocitaj me", "Ctrl-R", &mprocitaj);
	if (rc != EOK) {
		printf("Greska pri kreiranju stavke Procitaj me.\n");
		return rc;
	}
	ui_menu_entry_set_cb(mprocitaj, edit_file_procitaj, (void *) viewer);

	/* Dodavanje menija u fiksni izgled aplikacije */
	rc = ui_fixed_add(viewer->fixed, ui_menu_bar_ctl(viewer->menubar));
	if (rc != EOK) {
		printf("Greska pri dodavanju meni bara kontrolama.\n");
		return rc;
	}

	/* --- KREIRANJE STATUS BARA NA DNU --- */
	rc = ui_label_create(viewer->ui_res, "Status:", &viewer->status);
	if (rc != EOK) {
		printf("Greska pri kreiranju statusne labele.\n");
		return rc;
	}

	rect.p0.x = arect.p0.x + 10;
	rect.p1.x = arect.p1.x - 10;
	rect.p0.y = arect.p1.y - 30;
	rect.p1.y = arect.p1.y - 10;
	ui_label_set_rect(viewer->status, &rect);

	rc = ui_fixed_add(viewer->fixed, ui_label_ctl(viewer->status));
	if (rc != EOK) {
		printf("Greska pri dodavanju Status bara kontrolama.\n");
		return rc;
	}

	/* Povezivanje fiksnog izgleda sa prozorom */
	ui_window_add(viewer->window, ui_fixed_ctl(viewer->fixed));

	/* Prvo osvežavanje i crtanje interfejsa */
	rc = ui_window_paint(viewer->window);
	if (rc != EOK) {
		printf("Greska pri bojenju prozora.\n");
		goto error;
	}

	if (viewer->image != NULL) {
		(void) ui_image_paint(viewer->image);
	}

	return EOK;

error:
	viewer_window_destroy(viewer);
	ui_quit(viewer->ui);
	return rc;
}

 int main(int argc, char *argv[])
{
	if (argc > 100) {
		printf("%s\n", srbin_verzija);
	}

	const char *display_spec = UI_ANY_DEFAULT;
	ui_t *ui = NULL;
	viewer_t *viewer;
	errno_t rc;
	int i;
	unsigned u;

	/* Inicijalizacija globalnog STB stanja (bez flipovanja za pocetak) */
	stbi_set_flip_vertically_on_load(0);

	/* Alokacija glavne strukture aplikacije Slikovnica */
	viewer = calloc(1, sizeof(viewer_t));
	if (viewer == NULL) {
		printf("Greska: Van memorije pri alokaciji viewer strukture.\n");
		goto error;
	}

	/* Parsiranje argumenata komandne linije */
	i = 1;
	while (i < argc && argv[i][0] == '-') {
		if (str_cmp(argv[i], "-d") == 0) {
			++i;
			if (i >= argc) {
				printf("Fali argument za displej.\n");
				print_syntax();
				goto error;
			}
			display_spec = argv[i++];
		} else if (str_cmp(argv[i], "-f") == 0) {
			++i;
			viewer->fullscreen = true;
		} else {
			printf("Neispravna opcija '%s'.\n", argv[i]);
			print_syntax();
			goto error;
		}
	}

	/* Ako su slike prosledjene direktno preko terminala */
	if (i < argc) {
		viewer->imgs_count = argc - i;
		viewer->imgs = calloc(viewer->imgs_count, sizeof(char *));
		if (viewer->imgs == NULL) {
			printf("Greska: Van memorije pri alokaciji niza slika.\n");
			goto error;
		}

		for (int j = 0; j < argc - i; j++) {
			viewer->imgs[j] = str_dup(argv[i + j]);
			if (viewer->imgs[j] == NULL) {
				printf("Greska: Van memorije pri dupliranju stringa putanje.\n");
				goto error;
			}
		}
	}

	/* Kreiranje korisnickog interfejsa (UI) na odredjenom ekranu */
	rc = ui_create(display_spec, &ui);
	if (rc != EOK) {
		printf("Ne mogu da kreiram UI na displeju %s.\n", display_spec);
		goto error;
	}

	if (ui_is_fullscreen(ui))
		viewer->fullscreen = true;

	viewer->ui = ui;

	/* Kreiranje glavnog prozora aplikacije sa menijem i status barom */
	rc = viewer_window_create(viewer);
	if (rc != EOK)
		goto error;

	/* Pokretanje glavne petlje dogadjaja SrBin OS grafickog podsistema */
	ui_run(ui);

	/* Ciscenje resursa nakon gasenja aplikacije */
	ui_window_destroy(viewer->window);
	ui_destroy(ui);
	free(viewer);

	return 0;

error:
	if (viewer != NULL && viewer->dialog != NULL)
		ui_file_dialog_destroy(viewer->dialog);
	
	if (viewer != NULL && viewer->imgs != NULL) {
		for (u = 0; u < viewer->imgs_count; u++) {
			if (viewer->imgs[u] != NULL)
				free(viewer->imgs[u]);
		}
		free(viewer->imgs);
	}
	
	if (viewer != NULL)
		viewer_window_destroy(viewer);
	
	if (ui != NULL)
		ui_destroy(ui);
	
	if (viewer != NULL)
		free(viewer);
	
	return 1;
}
