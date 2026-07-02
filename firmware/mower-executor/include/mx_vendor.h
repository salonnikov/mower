/*
 * mx_vendor.h — ABI родной прошивки chip1 (GD32F305), которую переиспользует
 * исполнитель. ЗДЕСЬ И ТОЛЬКО ЗДЕСЬ собраны все абсолютные адреса/офсеты.
 *
 * Легенда достоверности (см. каталог docs/fw):
 *   [OK] — подтверждено дизасмом+сырыми байтами (несколько независимых проверок);
 *   [H]  — гипотеза/приближение, ОБЯЗАТЕЛЬНО подтвердить на живом чипе (SWD)
 *          перед первой сборкой-под-заливку. Помечено MX_VENDOR_UNCONFIRMED.
 *
 * Тела функций вызываются по абсолютному адресу (исполнитель живёт в том же
 * образе, что и родной код). Thumb-указатели — с установленным битом 0.
 */
#ifndef MX_VENDOR_H
#define MX_VENDOR_H

#include <stdbool.h>
#include <stdint.h>

/* Пометка «адрес не подтверждён на живом чипе». */
#define MX_VENDOR_UNCONFIRMED 1

/* --- Приводы колёс [OK: логика; H: точный вход функции] --- */
/* left_motor_rotation(obj, speed): speed 0..6141 знаковый. Вход ~0x0805f620. */
#define MX_ADDR_LEFT_MOTOR_ROTATION  0x0805f621U /* [H] thumb */
/* right_motor_rotation — точный вход не запинен (см. verification-log). */
#define MX_ADDR_RIGHT_MOTOR_ROTATION 0x00000000U /* [H] TODO: запинить на чипе */

/* Слот объекта драйвера левого колеса (lmdrv): *0x2000054c -> объект. [OK] */
#define MX_ADDR_LMDRV_SLOT 0x2000054cU
/* Слот rmdrv — TODO (симметрично lmdrv). */
#define MX_ADDR_RMDRV_SLOT 0x00000000U /* [H] TODO */

/* --- Главное состояние машины [OK] --- */
#define MX_ADDR_MAIN_STATE 0x200000bcU /* байт состояния 0..10 */

/* --- Одометрия (тики холлов) [OK: анкеры по живому SWD] --- */
#define MX_ADDR_ODOM_LEFT  0x2000be28U
#define MX_ADDR_ODOM_RIGHT 0x2000be58U

/* --- Process-manager (точка инъекции) [OK: анкеры резолвнуты из бинаря] --- */
#define MX_ADDR_PMGR_SLOT   0x20000078U /* *slot -> обёртка сервиса */
#define MX_PMGR_TICK_INDEX  0U          /* индекс метода-tick в обёртке [H] */

/* --- Watchdog (IWDG/FWDGT) [OK: периферия; H: функция кормления] --- */
#define MX_ADDR_IWDG        0x40003000U
/* Родная feed-функция (fcn.0804bca0) — [H], либо кормить регистром напрямую. */
#define MX_ADDR_WDT_FEED    0x00000000U /* [H] TODO */

/* Тип указателя на родную функцию привода. */
typedef int (*mx_motor_rotation_fn)(void *obj, int16_t speed, uint32_t dir_hint);

/* Снимок телеметрии, отдаётся наружу. */
typedef struct {
    uint8_t  state;      /* MX_ADDR_MAIN_STATE */
    int32_t  odom_l;     /* тики левого */
    int32_t  odom_r;     /* тики правого */
    int16_t  coil_l;     /* [H] боковая магнитуда (или брать с USART2 chip2) */
    int16_t  coil_r;     /* [H] */
    int16_t  heading;    /* [H] IMU курс */
    uint16_t batt_mv;    /* [H] */
    int16_t  cur_l;      /* [H] ток левого мотора, мА */
    int16_t  cur_r;      /* [H] */
    uint8_t  flags;      /* bit0 lift, bit1 hit, bit2 rain, bit3 button [H] */
} mx_telemetry_t;

/* Команда привода: знаковые скорости колёс (уже отфильтрованы микшером). */
void mx_vendor_drive(int16_t left, int16_t right);

/* Заглушка ножа (вкл/выкл + обороты). */
void mx_vendor_blade(bool on, uint16_t rpm);

/* Немедленный стоп приводов. */
void mx_vendor_stop(void);

/* Собрать телеметрию из известных адресов/объектов. */
void mx_vendor_read_telemetry(mx_telemetry_t *t);

/* Покормить watchdog (обязателен, если гасим родные кормильцы). */
void mx_vendor_feed_watchdog(void);

/* Монотонное время в мс (из системного тика). */
uint32_t mx_vendor_now_ms(void);

#endif /* MX_VENDOR_H */
