/******************************************************************************
 * @file           : sampler.c
 * @brief          : TIM7 interrupt every 20 ms, runs detection_step().
 *
 * TIM7 priority is 5 and SysTick is 0, so SysTick can still preempt us. That
 * matters because the HAL timeouts inside the sensor reads use HAL_GetTick().
 * Nothing in the ISR prints or draws - the main loop does that.
 ******************************************************************************/

/*--------------------------- Includes ---------------------------------------*/
#include "main.h"
#include "sampler.h"

/*--------------------------- Configuration ----------------------------------*/
#define SAMPLER_TIMER           TIM7
#define SAMPLER_IRQ             TIM7_IRQn
#define SAMPLER_PRIORITY        5U      /* SysTick is 0 */
#define SAMPLER_TICK_HZ      1000U      /* timer counts in ms */

/*--------------------------- State ------------------------------------------*/

static TIM_HandleTypeDef htim_sampler;
static volatile uint32_t steps;
static volatile uint32_t overruns;
static volatile uint8_t  busy;

/*--------------------------- The interrupt ----------------------------------*/

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != SAMPLER_TIMER)
    {
        return;
    }

    /* previous step still running -> skip this one and count it */
    if (busy)
    {
        overruns++;
        return;
    }

    busy = 1;
    detection_step();
    steps++;
    busy = 0;
}

/*--------------------------- Start-up ---------------------------------------*/

void sampler_start(void)
{
    uint32_t timer_clock;
    uint32_t prescaler;

    __HAL_RCC_TIM7_CLK_ENABLE();

    /* prescale the timer to 1 kHz so the period is just a number of ms
     * (APB1 timer clock is x2 when the APB1 prescaler isn't 1) */
    timer_clock = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U)
    {
        timer_clock *= 2U;
    }
    prescaler = (timer_clock / SAMPLER_TICK_HZ);
    if (prescaler > 0U)
    {
        prescaler--;
    }

    htim_sampler.Instance = SAMPLER_TIMER;
    htim_sampler.Init.Prescaler = prescaler;
    htim_sampler.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim_sampler.Init.Period = SAMPLER_PERIOD_MS - 1U;
    htim_sampler.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&htim_sampler) != HAL_OK)
    {
        return;
    }

    HAL_NVIC_SetPriority(SAMPLER_IRQ, SAMPLER_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(SAMPLER_IRQ);

    HAL_TIM_Base_Start_IT(&htim_sampler);
}

uint32_t sampler_steps(void)
{
    return steps;
}

uint32_t sampler_overruns(void)
{
    return overruns;
}

/*--------------------------- Interrupt entry --------------------------------*/

void TIM7_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim_sampler);
}
