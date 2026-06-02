#include <pulse/malloc.h>
#include <pulse/task.h>
#include <pulse/timer.h>
#include <pulse/port.h>
#include <pulse/token_queue.h>
#include <pulse/discard_queue.h>

#include <stm32f1xx_hal.h>
#include <stm32f103xb.h>


[[noreturn]] void
Panic(const char *msg)
{
    pulse::DisableInterrupts();
    for(;;);

    /* Stop in debug build, reset after delay in release build. */
#ifdef DEBUG
    for(;;);
#else
    /* Make delay before reset */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    if (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) {
        /* 1 second delay. */
        uint32_t cycles = SystemCoreClock;
        uint32_t start = DWT->CYCCNT;
        while ((DWT->CYCCNT - start) < cycles);
    }

    NVIC_SystemReset();
#endif
}

void
MallocLock()
{
    pulse::EnterCriticalSection();
}

void
MallocUnlock()
{
    pulse::ExitCriticalSection();
}

namespace {

struct GpioLine {
    uintptr_t port;
    uint16_t pin;

    GPIO_TypeDef *
    Port() const
    {
        return reinterpret_cast<GPIO_TypeDef *>(port);
    }
};

#define DEF_IO(port, pin) {GPIO ## port ## _BASE, GPIO_PIN_ ## pin}

constexpr GpioLine
    ioLed       DEF_IO(C, 13),
    ioRotEncA   DEF_IO(A, 10),
    ioRotEncB   DEF_IO(A, 11);

pulse::MallocUnit heap[PULSE_HEAP_UNITS_SIZE_KB(16)];

void
SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /** Initializes the RCC Oscillators according to the specified parameters
     * in the RCC_OscInitTypeDef structure.
     */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Panic("HAL_RCC_OscConfig failed");
    }
    /** Initializes the CPU, AHB and APB buses clocks
     */
    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
        Panic("HAL_RCC_ClockConfig failed");
    }
}

void
InitLed(void)
{
    GPIO_InitTypeDef init = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();

    // Initially off (active low)
    HAL_GPIO_WritePin(ioLed.Port(), ioLed.pin, GPIO_PIN_SET);

    init.Pin = ioLed.pin;
    init.Mode = GPIO_MODE_OUTPUT_OD;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ioLed.Port(), &init);
}

void
InitRotaryEncoder()
{
    GPIO_InitTypeDef init = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    init.Pin = ioRotEncA.pin;
    init.Mode = GPIO_MODE_IT_FALLING;
    init.Pull = GPIO_PULLUP;

    HAL_GPIO_Init(ioRotEncA.Port(), &init);

    init.Pin = ioRotEncB.pin;
    init.Mode = GPIO_MODE_IT_FALLING;
    init.Pull = GPIO_PULLUP;

    HAL_GPIO_Init(ioRotEncB.Port(), &init);

    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 8, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

void
LedOn()
{
    HAL_GPIO_WritePin(ioLed.Port(), ioLed.pin, GPIO_PIN_RESET);
}

void
LedOff()
{
    HAL_GPIO_WritePin(ioLed.Port(), ioLed.pin, GPIO_PIN_SET);
}


TIM_HandleTypeDef hTim3;
constexpr int PWM_BITS = 10;
constexpr uint16_t MAX_PWM = (1 << PWM_BITS) - 1;

void
SetPwm(uint16_t value)
{
#ifdef __clang__
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wdeprecated-volatile"
#endif
    __HAL_TIM_SET_COMPARE(&hTim3, TIM_CHANNEL_1, value);
#ifdef __clang__
#   pragma GCC diagnostic pop
#endif
}

uint16_t
GetPwm()
{
    return __HAL_TIM_GET_COMPARE(&hTim3, TIM_CHANNEL_1);
}

/** Calculate PWM value from linearly perceived brightness. Perception is approximately described as
 * `real_brightness ^ 0.33`, so just calculate `brightness ^ 3`.
 */
uint16_t
CalculatePwm(uint16_t brightness)
{
    if (brightness == 0) {
        return 0;
    }
    if (brightness >= MAX_PWM) {
        return MAX_PWM;
    }
    uint32_t pwm = static_cast<uint32_t>(brightness) * static_cast<uint32_t>(brightness) *
        static_cast<uint32_t>(brightness);
    return pwm >> (PWM_BITS * 2);
}

/** Approximate calculation of minimal value which will result in calculated PWM value 1 to
 * eliminate completely dark beginning.
 */
constexpr uint16_t MIN_BRIGHTNESS =
    (1 << (etl::bit_width(static_cast<uint32_t>(1) << (PWM_BITS * 2)) / 3)) - 1;

uint16_t curBrightness = MIN_BRIGHTNESS;
// Limit indication in progress if not empty.
pulse::TaskRef indicateMaxTask;

pulse::Task<>
IndicateMaxBrightness()
{
    LedOff();
    co_await pulse::Timer::Delay(etl::chrono::milliseconds(300));
    LedOn();
    co_await pulse::Timer::Delay(etl::chrono::milliseconds(300));
    indicateMaxTask.ReleaseHandle();
}

void
InitPwm()
{
    __HAL_RCC_TIM3_CLK_ENABLE();

    hTim3.Instance = TIM3;
    // We need 1kHz period.
    hTim3.Init.Prescaler = 72000000 / 1000 / (MAX_PWM + 1);
    hTim3.Init.CounterMode = TIM_COUNTERMODE_UP;
    hTim3.Init.Period = MAX_PWM;
    hTim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;

    HAL_TIM_Base_Init(&hTim3);
    HAL_TIM_OC_Init(&hTim3);

    TIM_OC_InitTypeDef sConfig = {0};
    // Do not control pin directly, built-in LED sits on incompatible pin.
    sConfig.OCMode = TIM_OCMODE_TIMING;
    sConfig.Pulse = CalculatePwm(curBrightness);
    sConfig.OCPolarity = TIM_OCPOLARITY_HIGH;

    HAL_TIM_OC_ConfigChannel(&hTim3, &sConfig, TIM_CHANNEL_1);

    HAL_TIM_Base_Start_IT(&hTim3);
    HAL_TIM_OC_Start_IT(&hTim3, TIM_CHANNEL_1);

    HAL_NVIC_SetPriority(TIM3_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(TIM3_IRQn);
}

extern "C" void
HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *hTim)
{
    if (hTim->Instance == TIM2) {
        HAL_IncTick();
        pulse::Timer::Tick();
    } else if (hTim->Instance == TIM3) {
        if (GetPwm() != 0 && !indicateMaxTask) {
            LedOn();
        }
    }
}

extern "C" void
HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *hTim)
{
    if (hTim->Instance == TIM3 && hTim->Channel == HAL_TIM_ACTIVE_CHANNEL_1 && !indicateMaxTask) {
        LedOff();
    }
}

extern "C" void
TIM3_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&hTim3);
}


class RotaryEncoder {
public:
    void
    Initialize();

    void
    OnLineInterrupt(bool isA);

    /** Get next click event. Value is number of clicks accumulated in given direction. Direction is
     * represented by sign.
     */
    pulse::Awaitable<int8_t>
    WaitClick()
    {
        co_return co_await clicks.Pop();
    }

private:
    pulse::TokenQueue<uint8_t> lineAEvent{5}, lineBEvents{5};
    etl::optional<bool> lastDir;
    bool lastLine = false, halfClick = false;
    pulse::InlineDiscardQueue<int8_t, true, 16> clicks;

    pulse::Task<>
    LineTask(bool isA);

    static bool
    GetLineState(bool isA)
    {
        GpioLine line = isA ? ioRotEncA : ioRotEncB;
        return HAL_GPIO_ReadPin(line.Port(), line.pin) == GPIO_PIN_RESET;
    }

    void
    CommitEvent(bool triggerLineA, bool adjLineState);

    void
    CommitClick(bool dir);
};

RotaryEncoder rotEnc;

void
RotaryEncoder::OnLineInterrupt(bool isA)
{
    (isA ? lineAEvent : lineBEvents).Push();
}

void
RotaryEncoder::Initialize()
{
    pulse::tasks::Spawn(LineTask(true), pulse::tasks::HIGHEST_PRIORITY).Pin();
    pulse::tasks::Spawn(LineTask(false), pulse::tasks::HIGHEST_PRIORITY).Pin();
}

pulse::Task<>
RotaryEncoder::LineTask(bool isA)
{
    constexpr auto JITTER_DELAY = etl::chrono::milliseconds(1);
    pulse::Timer jitterTimer;
    pulse::TokenQueue<uint8_t> &lineEvents = isA ? lineAEvent : lineBEvents;

    while (true) {
        co_await lineEvents;
        // Suppress jitter - wait until active level is stable for a long period.
        bool pressed = false;
        while (true) {
            jitterTimer.ExpiresAfter(JITTER_DELAY);
            size_t idx = co_await pulse::tasks::WhenAny(lineEvents, jitterTimer);
            if (idx == 0) {
                // Activated again, restart anti-jitter delay
                continue;
            }
            // Anti-jitter delay expired with no new events. Check if signal still active.
            if (!GetLineState(isA)) {
                break;
            }
            pressed = true;
            break;
        }

        if (!pressed) {
            // Ignore too short activation.
            continue;
        }
        CommitEvent(isA, GetLineState(!isA));
    }
}

void
RotaryEncoder::CommitEvent(bool triggerLineA, bool adjLineState)
{
    bool dir = triggerLineA == adjLineState;
    if (!lastDir || *lastDir != dir) {
        // First event or direction changed
        lastDir = dir;
        lastLine = triggerLineA;
        halfClick = true;
        return;
    }
    if (lastLine == triggerLineA) {
        // Lines should alternate if consistent data
        return;
    }
    lastLine = triggerLineA;
    if (halfClick) {
        // Second line trigger, full cycle detected
        CommitClick(dir);
        halfClick = false;
    } else {
        // First line trigger, wait for the second one
        halfClick = true;
    }
}

void
RotaryEncoder::CommitClick(bool dir)
{
    if (clicks.IsEmpty()) {
        clicks.Push(dir ? 1 : -1);
        return;
    }
    bool lastDir = clicks.PeekLast() > 0;
    if (dir == lastDir) {
        clicks.PeekLast() += dir ? 1 : -1;
    } else {
        clicks.Push(dir ? 1 : -1);
    }
}

pulse::Task<>
RotaryEncoderTask()
{
    while (true) {
        int16_t clicks = co_await rotEnc.WaitClick();
        if (indicateMaxTask) {
            // Limit indication is in progress
            continue;
        }
        int16_t newBrightness = static_cast<int16_t>(curBrightness) + (clicks << 4);
        if (newBrightness < MIN_BRIGHTNESS) {
            newBrightness = MIN_BRIGHTNESS;
        } else if (newBrightness > MAX_PWM) {
            newBrightness = MAX_PWM;
            indicateMaxTask = pulse::tasks::Spawn(IndicateMaxBrightness());
        }
        curBrightness = newBrightness;
        SetPwm(CalculatePwm(curBrightness));
    }
}

} /* anonymous namespace */

extern "C" void
EXTI15_10_IRQHandler()
{
    HAL_GPIO_EXTI_IRQHandler(ioRotEncA.pin);
    HAL_GPIO_EXTI_IRQHandler(ioRotEncB.pin);
}

void
HAL_GPIO_EXTI_Callback(uint16_t gpioPin)
{
    if (gpioPin == ioRotEncA.pin) {
        rotEnc.OnLineInterrupt(true);
    } else if (gpioPin == ioRotEncB.pin) {
        rotEnc.OnLineInterrupt(false);
    }
}

extern "C" [[noreturn]] int
main()
{
    pulse::AddHeapRegion(heap, sizeof(heap));

    HAL_Init();
    SystemClock_Config();

    InitLed();
    InitPwm();
    InitRotaryEncoder();
    rotEnc.Initialize();

    pulse::tasks::Spawn(RotaryEncoderTask()).Pin();

    pulse::tasks::RunScheduler();

    Panic("Scheduler exited");
}


#ifdef __clang__
extern "C" void
_init()
{}

extern "C" void
_fini()
{}
#endif // __clang__
