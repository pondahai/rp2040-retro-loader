#ifndef LAUNCH_H
#define LAUNCH_H

#include <stdbool.h>

/* APP_BASE 的向量表看起來像不像一份真的 image(而不是空白 flash)。 */
bool app_present(void);

/* 交棒給 APP_BASE 的專題本體。不返回。 */
void __attribute__((noreturn)) launch_app(void);

#endif /* LAUNCH_H */
