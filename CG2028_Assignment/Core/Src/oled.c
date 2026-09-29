/******************************************************************************
 * @file           : oled.c
 * @brief          : SSD1306 128x64 OLED text driver.
 *
 * I2C transfer: S | 0x78 | control byte | data... | P
 * control byte 0x00 = commands follow, 0x40 = pixel data follows.
 *
 * The panel is 8 pages of 128 bytes (each byte = 8 vertical pixels, bit 0 at
 * the top). We use one page per text row so rows can be updated separately.
 * A 128-byte row at 400 kHz is ~2.9 ms (9 bits per byte incl. ACK).
 ******************************************************************************/

/*--------------------------- Includes ---------------------------------------*/
#include "main.h"
#include "oled.h"
#include "oled_font.h"

#include <string.h>

/*--------------------------- Configuration ----------------------------------*/

/* 7-bit 0x3C / 0x3D shifted left. Our module is 0x3C but some are strapped to
 * 0x3D, so try both. */
#define OLED_ADDR_PRIMARY   0x78U
#define OLED_ADDR_ALT       0x7AU

#define OLED_CTRL_CMD       0x00U
#define OLED_CTRL_DATA      0x40U

/* 400 kHz from the 16 MHz HSI (62.5 ns per cycle):
 * SCLL = 29 -> 1875 ns low, SCLH = 9 -> 625 ns high, SCLDEL = 3, SDADEL = 1 */
#define OLED_I2C_TIMING     0x0031091DU

#define OLED_I2C_TIMEOUT_MS 10U         /* a row takes ~3 ms */

#define OLED_RETRY_MS       5000U       /* look for a missing display this often */

#define OLED_CELL_W         (OLED_FONT_WIDTH + 1)   /* 5 px + 1 px gap */
#define OLED_BIG_CELL_W     (2 * OLED_CELL_W)

/* SH1106 1.3" modules would need 132 / offset 2 */
#define OLED_WIDTH          128
#define OLED_COL_OFFSET     0

/*--------------------------- State ------------------------------------------*/

static I2C_HandleTypeDef hi2c_oled;

/* RAM copy of the screen */
static uint8_t frame[OLED_ROWS][OLED_WIDTH];

/* bit n set = row n needs sending */
static uint8_t dirty_rows;

/* 0 after a failed transfer, so a missing display doesn't slow the loop */
static uint8_t present;
static uint32_t next_probe_ms;

static uint8_t oled_addr = OLED_ADDR_PRIMARY;

/*--------------------------- Low level --------------------------------------*/

static HAL_StatusTypeDef oled_cmd(uint8_t command)
{
    return HAL_I2C_Mem_Write(&hi2c_oled, oled_addr, OLED_CTRL_CMD,
                             I2C_MEMADD_SIZE_8BIT, &command, 1,
                             OLED_I2C_TIMEOUT_MS);
}

uint8_t oled_probe(uint8_t wire_addr)
{
    return (HAL_I2C_IsDeviceReady(&hi2c_oled, wire_addr, 1,
                                  OLED_I2C_TIMEOUT_MS) == HAL_OK) ? 1U : 0U;
}

static uint8_t find_display(void)
{
    if (oled_probe(OLED_ADDR_PRIMARY))
    {
        oled_addr = OLED_ADDR_PRIMARY;
        return 1;
    }
    if (oled_probe(OLED_ADDR_ALT))
    {
        oled_addr = OLED_ADDR_ALT;
        return 1;
    }
    return 0;
}

uint8_t oled_address(void)
{
    return present ? oled_addr : 0U;
}

static void oled_failed(void)
{
    present = 0;
    next_probe_ms = HAL_GetTick() + OLED_RETRY_MS;
}

static void oled_write_row(uint8_t row)
{
    /* set page, then column low / high nibble */
    if ((oled_cmd(0xB0U | row) != HAL_OK) ||
        (oled_cmd(0x00U | (OLED_COL_OFFSET & 0x0FU)) != HAL_OK) ||
        (oled_cmd(0x10U | (OLED_COL_OFFSET >> 4)) != HAL_OK))
    {
        oled_failed();
        return;
    }

    if (HAL_I2C_Mem_Write(&hi2c_oled, oled_addr, OLED_CTRL_DATA,
                          I2C_MEMADD_SIZE_8BIT, frame[row], OLED_WIDTH,
                          OLED_I2C_TIMEOUT_MS) != HAL_OK)
    {
        oled_failed();
    }
}

/* I2C1 on PB8 (SCL) / PB9 (SDA). Separate from the sensor's I2C2. */
static HAL_StatusTypeDef oled_i2c_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_PeriphCLKInitTypeDef periph = {0};
    GPIO_InitTypeDef gpio = {0};

    /* core is on the 4 MHz MSI which is too slow for 400 kHz, so clock I2C1
     * from HSI16 instead (doesn't touch the other peripherals) */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        return HAL_ERROR;
    }

    periph.PeriphClockSelection = RCC_PERIPHCLK_I2C1;
    periph.I2c1ClockSelection = RCC_I2C1CLKSOURCE_HSI;
    if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK)
    {
        return HAL_ERROR;
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    /* open drain + internal pull-ups on top of the module's own */
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &gpio);

    hi2c_oled.Instance = I2C1;
    hi2c_oled.Init.Timing = OLED_I2C_TIMING;
    hi2c_oled.Init.OwnAddress1 = 0;
    hi2c_oled.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c_oled.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c_oled.Init.OwnAddress2 = 0;
    hi2c_oled.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c_oled.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c_oled.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    return HAL_I2C_Init(&hi2c_oled);
}

/*--------------------------- Drawing ----------------------------------------*/

/* returns 1 only if the byte actually changed, so redrawing the same text
 * doesn't mark the row dirty */
static uint8_t frame_set(uint8_t row, uint16_t x, uint8_t value)
{
    if ((x >= OLED_WIDTH) || (frame[row][x] == value))
    {
        return 0;
    }
    frame[row][x] = value;
    return 1;
}

static uint8_t draw_char(uint8_t row, uint16_t x, char c)
{
    uint8_t changed = 0;
    uint8_t i;

    if ((c < OLED_FONT_FIRST) || (c > OLED_FONT_LAST))
    {
        c = '?';
    }

    for (i = 0; i < OLED_FONT_WIDTH; i++)
    {
        changed |= frame_set(row, x + i, OLED_FONT[(uint8_t)c - OLED_FONT_FIRST][i]);
    }
    changed |= frame_set(row, x + OLED_FONT_WIDTH, 0x00);
    return changed;
}

/* double every bit: 8 px column -> 16 px (low byte = top row, high byte = next) */
static uint16_t stretch_column(uint8_t bits)
{
    uint16_t out = 0;
    uint8_t bit;

    for (bit = 0; bit < 8; bit++)
    {
        if (bits & (1U << bit))
        {
            out |= (uint16_t)0x3U << (bit * 2);
        }
    }
    return out;
}

/* double width and height, spans row and row + 1 */
static uint8_t draw_big_char(uint8_t row, uint16_t x, char c)
{
    uint8_t changed = 0;
    uint8_t i;

    if ((c < OLED_FONT_FIRST) || (c > OLED_FONT_LAST))
    {
        c = '?';
    }

    for (i = 0; i < OLED_FONT_WIDTH; i++)
    {
        uint16_t tall = stretch_column(OLED_FONT[(uint8_t)c - OLED_FONT_FIRST][i]);
        uint16_t x0 = x + (uint16_t)(i * 2);
        uint8_t half;

        for (half = 0; half < 2; half++)
        {
            changed |= frame_set(row, x0 + half, (uint8_t)(tall & 0xFFU));
            changed |= frame_set(row + 1, x0 + half, (uint8_t)(tall >> 8));
        }
    }

    /* 2 px gap */
    for (i = 0; i < 2; i++)
    {
        uint16_t gap = x + (uint16_t)(OLED_FONT_WIDTH * 2) + i;
        changed |= frame_set(row, gap, 0x00);
        changed |= frame_set(row + 1, gap, 0x00);
    }
    return changed;
}

void oled_text(uint8_t row, uint8_t col, const char *text)
{
    uint8_t changed = 0;
    uint8_t c;

    if (row >= OLED_ROWS)
    {
        return;
    }

    /* pad with spaces to the end of the row to clear old text */
    for (c = col; c < OLED_COLS; c++)
    {
        char ch = (*text != '\0') ? *text++ : ' ';
        changed |= draw_char(row, (uint16_t)c * OLED_CELL_W, ch);
    }

    if (changed)
    {
        dirty_rows |= (uint8_t)(1U << row);
    }
}

void oled_big(uint8_t row, const char *text)
{
    uint8_t changed = 0;
    uint8_t c;

    if ((row + 1) >= OLED_ROWS)
    {
        return;
    }

    for (c = 0; c < OLED_BIG_COLS; c++)
    {
        char ch = (*text != '\0') ? *text++ : ' ';
        changed |= draw_big_char(row, (uint16_t)c * OLED_BIG_CELL_W, ch);
    }

    if (changed)
    {
        dirty_rows |= (uint8_t)((1U << row) | (1U << (row + 1)));
    }
}

void oled_bar(uint8_t row, uint8_t percent, const char *label)
{
    char line[OLED_COLS + 1];
    uint8_t filled;
    uint8_t i;
    uint8_t n = 0;

    if (percent > 100U)
    {
        percent = 100U;
    }
    filled = (uint8_t)((percent + 5U) / 10U);    /* 10 cells */

    line[n++] = '[';
    for (i = 0; i < 10U; i++)
    {
        line[n++] = (i < filled) ? '#' : '-';
    }
    line[n++] = ']';
    line[n++] = ' ';

    while ((*label != '\0') && (n < OLED_COLS))
    {
        line[n++] = *label++;
    }
    line[n] = '\0';

    oled_text(row, 0, line);
}

void oled_clear(void)
{
    uint8_t row;
    uint16_t x;

    for (row = 0; row < OLED_ROWS; row++)
    {
        uint8_t changed = 0;

        for (x = 0; x < OLED_WIDTH; x++)
        {
            changed |= frame_set(row, x, 0x00);
        }
        if (changed)
        {
            dirty_rows |= (uint8_t)(1U << row);
        }
    }
}

/*--------------------------- Sending ----------------------------------------*/

void oled_service(void)
{
    uint8_t sent = 0;
    uint8_t row;

    /* display missing - only retry every OLED_RETRY_MS, each try costs a timeout */
    if (!present)
    {
        if ((int32_t)(HAL_GetTick() - next_probe_ms) < 0)
        {
            return;
        }
        if (!find_display())
        {
            next_probe_ms = HAL_GetTick() + OLED_RETRY_MS;
            return;
        }
        present = 1;
        dirty_rows = 0xFFU;     /* back again, redraw everything */
    }

    for (row = 0; (row < OLED_ROWS) && (sent < OLED_ROWS_PER_SERVICE); row++)
    {
        if (dirty_rows & (1U << row))
        {
            oled_write_row(row);
            if (!present)
            {
                return;
            }
            dirty_rows &= (uint8_t)~(1U << row);
            sent++;
        }
    }
}

void oled_flush(void)
{
    uint8_t guard = OLED_ROWS;

    while ((dirty_rows != 0U) && present && (guard-- > 0U))
    {
        oled_service();
    }
}

uint8_t oled_is_present(void)
{
    return present;
}

/*--------------------------- Start-up ---------------------------------------*/

void oled_init(void)
{
    /* SSD1306 init sequence */
    static const uint8_t setup[] = {
        0xAE,               /* display off */
        0xD5, 0x80,         /* clock divide */
        0xA8, 0x3F,         /* 64 rows */
        0xD3, 0x00,         /* no offset */
        0x40,               /* start line 0 */
        0x8D, 0x14,         /* charge pump on */
        0x20, 0x02,         /* page addressing */
        0xA1,               /* flip horizontally */
        0xC8,               /* flip vertically */
        0xDA, 0x12,         /* COM pins */
        0x81, 0x7F,         /* contrast */
        0xD9, 0xF1,         /* pre-charge */
        0xDB, 0x40,         /* VCOMH */
        0xA4,               /* display from RAM */
        0xA6,               /* not inverted */
        0xAF                /* display on */
    };
    uint8_t i;

    present = 0;
    next_probe_ms = 0;
    dirty_rows = 0;
    memset(frame, 0, sizeof(frame));

    if (oled_i2c_init() != HAL_OK)
    {
        return;
    }

    if (!find_display())
    {
        next_probe_ms = HAL_GetTick() + OLED_RETRY_MS;
        return;
    }
    present = 1;

    for (i = 0; i < sizeof(setup); i++)
    {
        if (oled_cmd(setup[i]) != HAL_OK)
        {
            oled_failed();
            return;
        }
    }

    /* display RAM is random at power-up, so push the blank frame once */
    oled_clear();
    dirty_rows = 0xFFU;
    oled_flush();
}
