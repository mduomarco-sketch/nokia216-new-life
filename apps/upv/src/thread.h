#pragma once

#define MAX_THEADS 10

#ifdef __cplusplus
extern "C" {
#endif

void thread_init(void);
void thread_create(unsigned int size,
                   void (*entrypoint)(void));
void thread_next(void);
void thread_end(void);

#ifdef __cplusplus
}
#endif
