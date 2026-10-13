
/** @addtogroup slikovnica
 * @{
 */
/** @file
 */

#ifndef SLIKOVNICA_H
#define SLIKOVNICA_H

#include <display.h>
#include <ui/checkbox.h>
#include <ui/entry.h>
#include <ui/fixed.h>
#include <ui/label.h>
#include <ui/list.h>
#include <ui/menu.h>
#include <ui/menuentry.h>
#include <ui/menubar.h>
#include <ui/menudd.h>
#include <ui/pbutton.h>
#include <ui/rbutton.h>
#include <ui/scrollbar.h>
#include <ui/slider.h>
#include <ui/tab.h>
#include <ui/tabset.h>
#include <ui/ui.h>
#include <ui/window.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <str.h>
#include <ui/filedialog.h>
#include <ui/image.h>
#include <ui/wdecor.h>




typedef struct {
	ui_t *ui;
	size_t imgs_count;
	size_t imgs_current;
	char **imgs;
	bool fullscreen;
	ui_window_t *window;
    	/** UI resource */
	ui_resource_t *ui_res;
    	/** Status bar */
	ui_label_t *status;
	gfx_bitmap_t *bitmap;
	ui_image_t *image;
	gfx_context_t *window_gc;
	ui_file_dialog_t *dialog;
	gfx_rect_t img_rect;
	char *file_name;
	const char *fname;
	ui_wnd_params_t params;
	int brojac;

	ui_fixed_t *fixed;
	ui_fixed_t *bfixed;
	ui_fixed_t *lfixed;
	ui_menu_bar_t *menubar;
	ui_menu_t *mfile;
	ui_menu_entry_t *mexit;
	ui_menu_t *medit;
	ui_menu_t *mpreferences;
	ui_menu_t *mhelp;
    
	ui_tab_set_t *tabset;
	ui_tab_t *tbasic;
	ui_tab_t *tlists;
	ui_entry_t *entry;
	ui_label_t *label;
	ui_pbutton_t *pb1;
	ui_pbutton_t *pb2;
	ui_checkbox_t *checkbox;
	ui_rbutton_group_t *rbgroup;
	ui_rbutton_t *rbleft;
	ui_rbutton_t *rbcenter;
	ui_rbutton_t *rbright;
	ui_slider_t *slider;
	ui_scrollbar_t *hscrollbar;
	ui_scrollbar_t *vscrollbar;
	ui_list_t *list;
	
	int ugao_rotacije;
} viewer_t;

int brojac=0;

double ldexp(double x, int exp);
/* =========================================================================
 * 2. EKSTERNE DEKLARACIJE IZ FAJLA meni.c (Rešava 'undeclared' greške u meniju)
 * ========================================================================= */
extern void edit_file_otvori(ui_menu_entry_t *mentry, void *arg);
extern void edit_file_exit(ui_menu_entry_t *mentry, void *arg);
extern void edit_file_rotiraj(ui_menu_entry_t *mentry, void *arg);
extern void edit_file_procitaj(ui_menu_entry_t *mentry, void *arg);
extern void edit_file_otvori(ui_menu_entry_t *mentry, void *arg);


unsigned char *get_cached_image(const char *src_path, int target_width, int target_height,
    int *out_width, int *out_height);
errno_t viewer_window_create(viewer_t *viewer);
void viewer_window_destroy(viewer_t *viewer);
void edit_file_exit(ui_menu_entry_t *mentry, void *arg);
void status_display(char const *str, void *arg);

void file_dialog_bok(ui_file_dialog_t *dialog, void *arg, const char *fname);
void file_dialog_bcancel(ui_file_dialog_t *dialog, void *arg);
void file_dialog_close(ui_file_dialog_t *dialog, void *arg);

unsigned char *convert_svg_to_rgb(const char *svg_text, int w, int h);
#endif