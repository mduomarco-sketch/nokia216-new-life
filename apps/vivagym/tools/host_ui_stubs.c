// Drawing/system calls vivagym.c references but the host test never runs (no MRE headers: signatures don't matter to the linker).
int vm_graphic_textout(void) { return 0; }
int vm_switch_power_saving_mode(void) { return 0; }
int vm_reg_sysevt_callback(void) { return 0; }
int vm_reg_keyboard_callback(void) { return 0; }
int vm_exit_app(void) { return 0; }
int vm_graphic_flush_layer(void) { return 0; }
int vm_graphic_get_screen_width(void) { return 0; }
int vm_graphic_get_screen_height(void) { return 0; }
int vm_graphic_create_layer(void) { return 0; }
int vm_graphic_get_layer_buffer(void) { return 0; }
int vm_graphic_set_clip(void) { return 0; }
int vm_graphic_get_character_height(void) { return 0; }
int vm_create_timer(void) { return 0; }
int vm_graphic_delete_layer(void) { return 0; }
int vm_delete_timer(void) { return 0; }
