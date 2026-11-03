#ifdef _WIN32
#ifndef _WINDRV_H_
#define _WINDRV_H_

void windrv_set_module_address(HMODULE module);
void windrv_set_vflip(BOOL flip);
void windrv_install_display_hook(void);

#endif
#endif
