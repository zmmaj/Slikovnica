/** @addtogroup slikovnica
 * @{
 */
/** @file
 */


#include "bmp.h"
  
  static errno_t napravi_meni(ui_t *, ui_window_t *);


   static errno_t napravi_meni(ui_t *ui, ui_window_t *prozor){
   ui_menu_entry_t *mbar;
   viewer_t meni;
   errno_t rc;
  
	rc = ui_menu_bar_create(ui, prozor, &meni.mbar);
	if (rc != EOK) {
		printf("Greska pri kreiranju menija bar.\n");
		return rc;
	}

   }




    /** @}
 */