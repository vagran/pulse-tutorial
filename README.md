# Pulse tutorial

This is tutorial for [Pulse framework](https://github.com/vagran/pulse).

Let's create a simple project using Pulse framework. We will make a rotary encoder example with a
STM32F103C8T6 MCU development board ("Blue pill"). It will decode encoder signals to direction and
position change signals, and then applying them to control built-in LED brightness. It also will
suppress input lines jittering in software. Despite this MCU has dedicated hardware for this kind of
tasks, we will do it in software to demonstrate Pulse capabilities. When brightness reaches maximum,
it should indicate it by short blink.


## Environment

_In this tutorial it is assumed that Linux host is used where it matters._


### Compiler

For GCC you typically need to install `arm-none-eabi-gcc` package on your distributive. Clang always
support cross-compilation for all its variety of targets, so just `clang` package is needed if using
Clang. However, it lacks of target sysroot, so it still needs target sysroot which is typically
located in `/usr/arm-none-eabi` and provided by packages like `arm-none-eabi-binutils` and
`arm-none-eabi-newlib`. It also needs some GCC runtime support files in sysroot so
`arm-none-eabi-gcc` package still need to be installed.


## Project setup

### Project layout

We will use the following project directories layout:
```
+- src
|  +- app
|     +- msp
|  +- STM32CubeF1
+- modules
   +- etl
   +- pulse
```
[`src/app`](./src/app) will contain all source code for the tutorial application.
[`modules`](./modules) used for Git submodules with dependencies.


#### STM32Cube

We will use STM32Cube SDK provided by the vendor. The base package and patch can be downloaded
[here](https://www.st.com/en/embedded-software/stm32cubef1.html). They should be extracted, patch
should override base package. I prefer manually select necessary files from SDK (other option is
installing STM32CubeMX software and generating code from there). We will need assembly entry point
for our MCU in `Core/Startup`, CMSIS drivers in `Drivers/CMSIS/Device/STM32F1xx` and HAL drivers in
`Drivers/STM32F1xx_HAL_Driver`. See full layout in the [repository](./src/STM32CubeF1).


### Toolchain

In this tutorial we can use either Clang or GCC cross-compiler. We will use CMake toolchain files
functionality to define the toolchain. See toolchain files for [Clang](./arm-clang-toolchain.cmake)
and [GCC](./arm-gcc-toolchain.cmake) in the repository.


### Dependency modules

The dependencies should be added as git submodules in `modules` directory. We need
[ETL](https://github.com/ETLCPP/etl) as C++ standard templates library:

```bash
git submodule add --depth 1 https://github.com/ETLCPP/etl modules/etl
```

You also might want to checkout specific release tag:
```bash
cd modules/etl
git fetch --depth 1 --tags origin 20.45.0
git checkout 20.45.0
```
_Currently 20.46 and 20.47 releases seems to be [broken](https://github.com/ETLCPP/etl/issues/1404)._

The same for [Pulse](https://github.com/vagran/pulse) framework:
```bash
git submodule add --depth 1 https://github.com/vagran/pulse modules/pulse
```


### Makefile

We will use CMake for building.

```cmake
set(CMAKE_CXX_STANDARD 20)
```
Pulse requires at least C++20 for coroutines support.

```cmake
add_compile_options(-mcpu=cortex-m3 -mthumb)
add_link_options(-mcpu=cortex-m3 -mthumb)
```
Set Cortex-M3 target with Thumb support.

```
add_compile_definitions(STM32F103xB)
```
This preprocessor symbol tells the STM32Cube SDK proper MCU model.

```cmake
add_link_options(-Wl,-gc-sections,--print-memory-usage,-Map=${PROJECT_BINARY_DIR}/${PROJECT_NAME}.map)
```
Some useful linker options. `-ffunction-sections` compile option specified before that, causes
compiler to place each function in its own separated section, while `-gc-sections` linker option
instructs linker to discard all unused section. This results that all unused functions are discarded
from the resulted binary. Other options allow printing memory usage by segments and dump full memory
map which may be useful for debugging.

```cmake
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/src/STM32F103CBTX_FLASH.ld)
add_link_options(-T ${LINKER_SCRIPT})
```
Use custom linker script.

```cmake
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
```
C++ exceptions and RTTI are not used in Pulse, and in general rarely used in embedded applications.

```cmake
set(PULSE_PORT ARM_CM3)
add_subdirectory("${CMAKE_SOURCE_DIR}/modules/pulse/src" "${CMAKE_BINARY_DIR}/pulse")
target_link_libraries(${PROJECT_NAME} PRIVATE pulse::pulse)
```
This is how link Pulse submodule to your project. Built-in port `ARM_CM3` is specified.

Let's also handle debug build by disabling optimizations for easier debugging and defining `DEBUG`
preprocessor symbol to detect debug build in code and, e.g. enable assertions:
```cmake
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -fno-omit-frame-pointer")
    add_compile_definitions(DEBUG)
endif()
```

Now you can build the project by these commands:
```bash
cmake -DCMAKE_TOOLCHAIN_FILE=arm-clang-toolchain.cmake -B build -G "Unix Makefiles"
cmake --build build
```

## Source code

Here is what we have in [`src/app`](src/app).

### Linker script

See [linker script file](src/STM32F103CBTX_FLASH.ld). This is mostly generated by STM32CubeIDE. Pay
attention to this line:
```
_Min_Heap_Size = 0;
```
It does not reserve space for heap in way used by typical libc implementation. Instead we will
define our heap in code and register it in Pulse memory allocator.

```
_Min_Stack_Size = 0x400; /* required amount of stack */
```
This line defines how much stack space is reserved for the application.


### Configuration files

Some components are configured by providing header files with necessary preprocessor definitions.


#### STM32Cube HAL configuration

[`stm32f1xx_hal_conf.h`](src/app/stm32f1xx_hal_conf.h) files defines which HAL drivers are enabled
and their parameters. You should review and edit list of enabled modules, you typically need some
essentials like clock control, GPIO, external interrupts, timers which are enabled by
`HAL_RCC_MODULE_ENABLED`, `HAL_GPIO_MODULE_ENABLED`, `HAL_EXTI_MODULE_ENABLED` and
`HAL_TIM_MODULE_ENABLED`.

Also proper clock sources should be defined here. "Blue pill" has 8MHz oscillator so `HSE_VALUE`
should be set to `8000000`.


### ETL configuration

ETL searches for [`etl_profile.h`](src/app/etl_profile.h) file to get its configuration. The only
required value there is `ETL_NO_STL` which tells that it is completely standalone STL replacement.


### Pulse configuration

Pulse searches for [`pulse_config.h](src/app/pulse_config.h) file for its configuration.

First thing you should deal with is memory allocation configuration. C++ coroutines require
dynamic allocation for coroutine frame so this is essential. Fortunately, Pulse includes highly
optimized memory allocator suitable for any embedded target. It can be configured for particular
memory constraints by adjusting these core parameters: `pulseConfig_MALLOC_GRANULARITY` and
`pulseConfig_MALLOC_BLOCK_SIZE_WORD_SIZE`. Granularity defines smallest memory allocation units and
simultaneously its alignment. Value `8` works best for most cases however you might want to adjust
it if having too small or too big RAM. `pulseConfig_MALLOC_BLOCK_SIZE_WORD_SIZE` is a bit tricky, it
defines size in bytes of memory block size word. For example, value `2` means block size should be
represented by 16-bits unsigned integer which reflects number of allocation units. Having
granularity of 8 bytes and word size 2 means the maximal size of allocated block would be
`8 * 65536 = 524288`. Actually it's a bit less due to block overhead, but close to that. Actual
value can be obtained by `get_malloc_max_size()` function in run time if needed. Each allocated
block has two size fields, so in this example, it will have `2 * 2 = 4` bytes overhead. So balancing
these two values you can define most optimal memory allocator configuration in terms of allocation
overhead and maximal block size for you exact RAM constraints. Having
`pulseConfig_MALLOC_BLOCK_SIZE_WORD_SIZE = 1` will get you just 2 bytes overhead per block which may
be useful if having, e.g. 512 bytes of RAM. Correctness of the parameters combinations is validated
by Pulse in compile time.

Defining `#define pulseConfig_MALLOC_FAILED_PANIC 1` allows failing fast on memory allocation
failure instead of just returning null pointer.

`#define pulseConfig_MALLOC_STATS 1` enables memory allocation statistics gathering and obtaining it
by `pulse::GetMallocStats()` function.

If you want to have dynamic memory allocation in ISR context (which sometimes might be needed if
using in ISR Pulse functions which are marked is ISR-safe), you need to provide
`pulseConfig_MALLOC_LOCK` and `pulseConfig_MALLOC_UNLOCK` macros. For example:
```cpp
#ifdef __cplusplus
extern "C" {
#endif

void
MallocLock();

void
MallocUnlock();

#ifdef __cplusplus
}
#endif

#define pulseConfig_MALLOC_LOCK()                   MallocLock()
#define pulseConfig_MALLOC_UNLOCK()                 MallocUnlock()
```
`MallocLock()` and `MallocUnlock()` functions should ensure no interrupt can occur between these
calls. They can use Pulse critical section API, for example.

To use timers you should define tick frequency by
```cpp
#define pulseConfig_TICK_FREQ                       1000
```
`pulse::Timer::Tick()` function (or C version `PulseTimerTick()`) should be called with this
frequency.

`pulseConfig_MAX_SYSCALL_INTERRUPT_PRIORITY` defines highest interrupt priority under which ISR-safe
Pulse functions can be called. You should never call any Pulse function in higher priority ISR code.

Pulse config code has internal validation by using asserts in critical places. You can enable them
by defining `pulseConfig_ASSERT` macro. Typically you want to do this only in debug build to
eliminate performance and code size impact in release build:
```cpp
#ifdef DEBUG
#   define pulseConfig_ASSERT(x) do { \
        if (!(x)) { \
            Panic("pulseConfig_ASSERT failed: " PULSE_STR(x)); \
        } \
    } while (false)
#else
#   define pulseConfig_ASSERT(x)
#endif
```
The macro takes checked condition argument.


### System calls stubs

libc implementation like `newlib` requires some basic system calls to be available. If your project
does not rely on libc provided features, most of them can be just empty stubs. They are defined in
[`syscals.c`](src/app/syscalls.c) file.


### MCU support package

The approach enforced by STM32Cube requires you to define hardware configuration by adjusting set of
functions called "MCU support package" (MSP). This code is located in [`src/app/msp`](src/app/msp).
This is mostly generated by STM32Cube. The only adjustment needed there is calling
`PulseTimerTick()` for each system tick:
```cpp
void
HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    HAL_IncTick();
    PulseTimerTick();
}
```


### Application

Finally we can have hands on our application code.

First thing to do when Pulse application starts, is providing heap space to allow dynamic memory
allocations. We define our heap space as follows:
```cpp
pulse::MallocUnit heap[PULSE_HEAP_UNITS_SIZE_KB(16)];
```
`pulse::MallocUnit` is a helper type which size and alignment corresponds to current memory
allocator configuration so it is the easiest way to define heap space. `PULSE_HEAP_UNITS_SIZE*`
macros are complementary helpers for defining resulting heap size. Here we define 16KB, however this
particular application requires just hundreds of bytes for coroutines frames - equivalent to what
would be otherwise allocated on stack in a traditional application.

Note that memory allocator has also C API which can be used from C files or as standalone embedded
memory allocator library.

Then the allocated space is fed to the Pulse memory manager:
```cpp
pulse::AddHeapRegion(heap, sizeof(heap));
```
You can add multiple regions if you want. This also can be done at any time after application
started. This may be handy if your MCU has several RAM banks (like TCM RAM regions on STM32 MCUs).

Here we define synchronization function for memory manager, specified earlier in `pulse_config.h`:
```cpp
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
```

Let's also provide `Panic()` function to allow catching unexpected error conditions:
```cpp
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
```
It will stop anything in endless loop, waiting for debugger be attached in debug build. In release
it will reset the MCU with some delay. You may want also to turn on some fault LED and flush debug
console UART in your application.

Next, define out I/O:
```cpp
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
```
Builtin LED is connected to `PC13`. Rotary encoder line `A` should be connected to `PC10`, line `B`
to `PC11`, ground pin to ground.

Initialize HAL and clock by calling STM32Cube functions:
```cpp
HAL_Init();
SystemClock_Config();
```

LED GPIO initialization:
```cpp
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
```

LED anode is connected to `VCC`, cathode to `PC13` pin via a resistor, so we configure the pin as
`open drain` type and it will have active level low, which is reflected in the following functions:
```cpp
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
```

PWM is configured using `TIM3`. The LED pin `PC13` cannot be used as PWM output, so we will
implement software PWM - LED pins will be toggled in timer ISR. It is configured at 1kHz frequency
and 10 bits resolution.

Let's examine rotary encoder implementation code. Start from getting input from encoder lines in
ISR:
```cpp
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

// Defined in RotaryEncoder class
pulse::TokenQueue<uint8_t> lineAEvent{5}, lineBEvents{5};

void
RotaryEncoder::OnLineInterrupt(bool isA)
{
    (isA ? lineAEvent : lineBEvents).Push();
}
```
Here it uses `TokenQueue` class to publish line falling edge events. Each token is numeric event ID,
which is incremented with each event. Constructor argument is limit for tokens which can be queued
by `Push()` method for retrieving later by `Take()` of `Peek()` methods. Excessive tokens are
discarded. `Push()` method can be called from ISR and is synchronous, while `Take()` is asynchronous
- it can be awaited in coroutine. This class is a light-weight way for propagating parameter-less
events from ISRs or any other context to coroutines. For events with parameters or any other objects
to queue, `DiscardQueue` can be used. For communication between coroutines - `BlockingQueue`.


Encoder initialization method spawns two tasks, one for processing events from each encoder line:
```cpp
void
RotaryEncoder::Initialize()
{
    pulse::Task::Spawn(LineTask(true), pulse::Task::HIGHEST_PRIORITY).Pin();
    pulse::Task::Spawn(LineTask(false), pulse::Task::HIGHEST_PRIORITY).Pin();
}
```
You should use `pulse::Task::Spawn()` method to spawn new coroutine (task). The method registers the
task in Pulse scheduler and it will be invoked once the scheduler will switch to next ready task.
Tasks scheduled according to priority.

Task handle returned by `Spawn()` method is basically a shared pointer to coroutine frame. The
coroutine is destroyed when last reference is released. The scheduler holds reference only to
scheduled or active tasks. Once a task reaches suspension point, schedular do not have the reference
to it. Awaiter usually also does not hold reference (it uses weak pointer) in order to prevent
reference loop. So to prevent task from destruction on first suspension point, either store the task
handle for its lifetime, or alternative use `Pin()` method to pin the task, i.e. prevent it from
destruction even after last reference is released. Awaiting task result also holds reference. Be
careful with pinning short repetitive tasks, they either should be unpinned by `Unpin()` method or
preferably store the handle. Use `Pin()` method for tasks which are created once and for the application
lifetime, when their handle is not needed, like in this example - encoder lines handling tasks will
run forever, handling line events:
```cpp
pulse::TaskV
RotaryEncoder::LineTask(bool isA)
{
    constexpr auto JITTER_DELAY = etl::chrono::milliseconds(1);
    pulse::Timer jitterTimer;

    while (true) {
        co_await (isA ? lineAEvent : lineBEvents);
        // Suppress jitter - wait until active level is stable for a long period.
        bool pressed = false;
        while (true) {
            jitterTimer.ExpiresAfter(JITTER_DELAY);
            size_t idx = co_await pulse::Task::WhenAny((isA ? lineAEvent : lineBEvents), jitterTimer);
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
```
This is task function, which is defined by its return type. `TaskV` is used for tasks with void
result, `TTask<TRet>` used when some result is returned. Both derived from `Task` base class which
is result-independent coroutine handle. Here it first awaits for next event from corresponding line
token queue. Once falling edge is detected it starts anti-jitter delay timer. We need to ensure
signal is stable during the delay duration. Timer object can be awaited for implementing a delay in
coroutine. Simple delays also can be performed by `Timer::Delay()` static method. Here by
`Task::WhenAny()` method we can aggregate awaiting on multiple awaiters. It returns index of the
first ready awaiter. If you need to wait for all awaiters to be ready, use `Task::WhenAll()` method.
Both can accept tasks, awaiters or awaitable objects (ones having operator `co_await` defined for
them). In this code, if first awaiter becomes ready (which is line event), it means line has changed
state before anti-jitter delay expired, so the delay restarts. If delay expires first, then there
was not falling edge detected, however rising edges are not detected, so it is checked if line is
still in active state (low). If it is, filtered line event is committed.

```cpp
void
RotaryEncoder::CommitEvent(bool triggerLineA, bool adjLineState)
{
    bool dir = triggerLineA == adjLineState;
    if (!lastDir || *lastDir != dir) {
        lastDir = dir;
        lastLine = triggerLineA;
        halfClick = true;
        return;
    }
    if (lastLine == triggerLineA) {
        return;
    }
    lastLine = triggerLineA;
    if (halfClick) {
        CommitClick(dir);
        halfClick = false;
    } else {
        halfClick = true;
    }
}
```
Here rotation direction is determined by checking other line state during detected falling edge.
Typical encoder click corresponds to full cycle - i.e. both lines transitioned to active state with
the same detected direction.

```cpp
// Defined in RotaryEncoder class
pulse::InlineDiscardQueue<int8_t, true, 16> clicks;

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
```
Here we use discard queue to further propagate detected clicks. The queue stores accumulated number
of clicks in one direction, direction is indicated by sign. Base `DiscardQueue` class accepts
external storage for queue content which might be dynamically or statically allocated.
`InlineDiscardQueue` (and other classes with `Inline` prefix in name) embeds the storage of fixed
size (specified in template parameter). The resulting stream of detected clicks is exposed in the
class interface like this:
```cpp
/** Get next click event. Value is number of clicked accumulated in given direction. Direction
 * represented by sign.
 */
pulse::Awaitable<int8_t>
WaitClick()
{
    co_return co_await clicks.Pop();
}
```
You can define any coroutine by specifying return type `pulse::Awaitable`. It is very similar to
`Task` but differs by initial suspension mode - `Task` body code starts execution only when task
starts by the scheduler, in contrast `Awaitable` is started synchronously until first suspension
point is reached and suspended (if corresponding awaiter is not instantly ready). `Task::Spawn()`
method can accept only `Task` (and its derived `TaskV` and `TTask`), `Awaitable` should be used for
directly invoked functions.

Here is tasks which processes encoder clicks:
```cpp
uint16_t curBrightness = MIN_BRIGHTNESS;
// Limit indication in progress if not empty.
pulse::Task indicateMaxTask;

pulse::TaskV
RotaryEncoderTask()
{

    while (true) {
        int16_t clicks = co_await rotEnc.WaitClick();
        if (indicateMaxTask) {
            continue;
        }
        int16_t newBrightness = static_cast<int16_t>(curBrightness) + (clicks << 4);
        if (newBrightness < MIN_BRIGHTNESS) {
            newBrightness = MIN_BRIGHTNESS;
        } else if (newBrightness > MAX_PWM) {
            newBrightness = MAX_PWM;
            indicateMaxTask = pulse::Task::Spawn(IndicateMaxBrightness());
        }
        curBrightness = newBrightness;
        SetPwm(CalculatePwm(curBrightness));
    }
}
```
It adjusts current brightness value correspondingly. Resulting PWM is calculated by `CalculatePwm()`
which introduces non-linearity to better match non-linear brightness perception by human eye. We
also spawn additional task to indicate maximal limit reaching. While it is active, brightness
adjustment is blocked.

```cpp
pulse::TaskV
IndicateMaxBrightness()
{
    LedOff();
    co_await pulse::Timer::Delay(etl::chrono::milliseconds(300));
    LedOn();
    co_await pulse::Timer::Delay(etl::chrono::milliseconds(300));
    indicateMaxTask.ReleaseHandle();
}
```
`Timer::Delay()` is used for simple delays. `Task::ReleaseHandle()` makes task handle empty also
releasing reference it holds to the task - in such way it unblocks further brightness adjustments by
encoder clicks.

Final piece is spawning top-level application tasks and starting scheduler in `main()` function:
```cpp
rotEnc.Initialize();

pulse::Task::Spawn(RotaryEncoderTask()).Pin();

pulse::Task::RunScheduler();

Panic("Scheduler exited");
```
This is should be your typical `main()` pattern. `Task::RunScheduler()` should never exit, it runs
all scheduled tasks according to their priorities. If there are no more tasks in ready state, it puts
MCU into low-power sleep mode.
