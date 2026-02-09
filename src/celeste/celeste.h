#ifndef CELESTE_H_
#define CELESTE_H_

#ifdef __cplusplus
#define _Bool bool
extern "C" {
#endif

extern void Celeste_P8_set_rndseed(unsigned seed);
extern void Celeste_P8_init(void);
extern void Celeste_P8_update(void);
extern void Celeste_P8_draw(void);

extern void Celeste_P8__DEBUG(void); //debug functionality

//state functionality
size_t Celeste_P8_get_state_size(void);
void Celeste_P8_save_state(void* st);
void Celeste_P8_load_state(const void* st);

#ifdef __cplusplus
} //extern "C"
#endif

#endif
