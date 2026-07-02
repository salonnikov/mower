/*
 * mx_vendor.c — тонкие обёртки над родным драйверным слоем chip1.
 *
 * ВНИМАНИЕ: адреса из mx_vendor.h с пометкой [H] не подтверждены на живом
 * чипе. Все вызовы по абсолютному адресу защищены проверкой на ноль, чтобы
 * несобранный/неподтверждённый анкер не приводил к вызову по NULL. До первой
 * сборки-под-заливку каждый [H] подтверждается по SWD (см. docs/fw/verification-log.md).
 */
#include "mx_vendor.h"

/* Доступ к абсолютным адресам как к volatile-регистрам/памяти. */
#define MX_R8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define MX_R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define MX_PTR(a) (*(void *volatile *)(uintptr_t)(a))

/* [H] адрес счётчика тиков FreeRTOS (мс при 1 кГц). 0 => плейсхолдер. */
#define MX_ADDR_FREERTOS_TICKS 0x00000000U

void mx_vendor_drive(int16_t left, int16_t right)
{
    if (MX_ADDR_LEFT_MOTOR_ROTATION != 0U && MX_ADDR_LMDRV_SLOT != 0U) {
        void *obj = MX_PTR(MX_ADDR_LMDRV_SLOT);
        if (obj != (void *)0) {
            mx_motor_rotation_fn fn =
                (mx_motor_rotation_fn)(uintptr_t)MX_ADDR_LEFT_MOTOR_ROTATION;
            (void)fn(obj, left, 0U);
        }
    }
    if (MX_ADDR_RIGHT_MOTOR_ROTATION != 0U && MX_ADDR_RMDRV_SLOT != 0U) {
        void *obj = MX_PTR(MX_ADDR_RMDRV_SLOT);
        if (obj != (void *)0) {
            mx_motor_rotation_fn fn =
                (mx_motor_rotation_fn)(uintptr_t)MX_ADDR_RIGHT_MOTOR_ROTATION;
            (void)fn(obj, right, 0U);
        }
    }
}

void mx_vendor_blade(bool on, uint16_t rpm)
{
    /* [H] TODO: вход драйвера ножа (service_blade). Пока no-op. */
    (void)on;
    (void)rpm;
}

void mx_vendor_stop(void)
{
    mx_vendor_drive(0, 0);
    mx_vendor_blade(false, 0U);
}

void mx_vendor_read_telemetry(mx_telemetry_t *t)
{
    t->state  = MX_R8(MX_ADDR_MAIN_STATE);
    t->odom_l = (int32_t)MX_R32(MX_ADDR_ODOM_LEFT);
    t->odom_r = (int32_t)MX_R32(MX_ADDR_ODOM_RIGHT);
    /* [H] остальные поля — через vtable сервис-объектов, добавляется после
     * подтверждения офсетов на чипе. Пока нули. */
    t->coil_l  = 0;
    t->coil_r  = 0;
    t->heading = 0;
    t->batt_mv = 0U;
    t->cur_l   = 0;
    t->cur_r   = 0;
    t->flags   = 0U;
}

void mx_vendor_feed_watchdog(void)
{
    if (MX_ADDR_WDT_FEED != 0U) {
        void (*feed)(void) = (void (*)(void))(uintptr_t)MX_ADDR_WDT_FEED;
        feed();
    } else if (MX_ADDR_IWDG != 0U) {
        /* Прямая запись ключа перезагрузки IWDG (FWDGT_CTL = 0xAAAA). */
        MX_R32(MX_ADDR_IWDG) = 0x0000AAAAU;
    } else {
        /* нечем кормить — оставляем родным задачам */
    }
}

uint32_t mx_vendor_now_ms(void)
{
    if (MX_ADDR_FREERTOS_TICKS != 0U) {
        return MX_R32(MX_ADDR_FREERTOS_TICKS);
    }
    /* [H] плейсхолдер до подтверждения адреса тика: монотонный счётчик.
     * НЕ калиброван по времени — заменить на реальный тик перед заливкой. */
    static uint32_t fake_ms;
    fake_ms++;
    return fake_ms;
}
