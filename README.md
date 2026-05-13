# Pulse tutorial

This is tutorial for [Pulse framework](https://github.com/vagran/pulse).

Let's create a simple project using the Pulse framework. In this example, we will use an
STM32F103C8T6 development board ("Blue Pill") to implement a rotary encoder interface.

The application will decode the encoder signals into direction and position changes, and use them to
control the brightness of the on-board LED. To improve signal quality, input jitter will be
suppressed in software. Although this MCU provides dedicated hardware for handling rotary encoders,
we will implement the logic in software to better demonstrate Pulse capabilities.

When the brightness reaches its maximum level, the system will indicate this with a short blink.


## Environment

_This tutorial assumes a Linux host environment where applicable._


### libc

Pulse, ETL, and STM32Cube depend on `libc` for basic facilities such as standard headers (e.g.
`stdint.h`). In embedded environments, `newlib` is the most commonly used `libc` implementation and
is typically provided by the `arm-none-eabi-newlib` package.


### Compiler

For GCC, you will typically need to install the `arm-none-eabi-gcc` package provided by your
distribution.

Clang, on the other hand, supports cross-compilation for a wide range of targets out of the box, so
installing the `clang` package is sufficient for the compiler itself.

In addition, Clang relies on certain GCC runtime support files within the sysroot, so the
`arm-none-eabi-gcc` package is still required. The example Clang toolchain file also uses the LLVM
implementation of `binutils` and `lld` linker, so the `llvm` and `lld` packages must be installed as
well.

In practice, your setup will look something like this (adjust as needed for your distribution):
```bash
sudo pacman -Sy clang llvm lld arm-none-eabi-gcc arm-none-eabi-newlib
```


## Project setup


### Project layout

We will use the following project directory layout:
```
+- src
|  +- app
|     +- msp
|  +- STM32CubeF1
+- modules
   +- etl
   +- pulse
```
The [`src/app`](./src/app) directory will contain all source code for the tutorial application.

The [`modules`](./modules) directory is used for Git submodules that provide external dependencies.


#### STM32Cube

We will use the STM32Cube SDK provided by the vendor. The base package and its patch can be
downloaded [here](https://www.st.com/en/embedded-software/stm32cubef1.html). After downloading,
extract both archives and apply the patch over the base package.

In this tutorial, we will manually select only the required files from the SDK. An alternative
approach is to use STM32CubeMX to generate the project automatically.

Specifically, we will use:
- the assembly startup files for our MCU from `Core/Startup`,
- CMSIS device headers from `Drivers/CMSIS/Device/STM32F1xx`,
- HAL drivers from `Drivers/STM32F1xx_HAL_Driver`.

You can refer to the complete layout in the [repository](./src/STM32CubeF1).


### Toolchain

In this tutorial, you can use either the Clang or GCC cross-compiler. The toolchain will be
configured using CMake toolchain files.

Refer to the provided toolchain files for [Clang](./arm-clang-toolchain.cmake) and
[GCC](./arm-gcc-toolchain.cmake) in the repository.


### Dependency modules

Add external dependencies as Git submodules in the `modules` directory.

We will use [ETL](https://github.com/ETLCPP/etl) as a lightweight C++ template library:
```bash
git submodule add --depth 1 https://github.com/ETLCPP/etl modules/etl
```
Optionally, you can check out a specific release tag:
```bash
cd modules/etl
git fetch --depth 1 --tags origin 20.45.0
git checkout 20.45.0
```
_Currently, versions 20.46 and 20.47 appear to be broken (see [issue
#1404](https://github.com/ETLCPP/etl/issues/1404))._
Pulse uses ETL extensively in both its implementation and public API, so it is strongly recommended
to use ETL in your application as well.

Add the [Pulse](https://github.com/vagran/pulse) framework in the same way:
```bash
git submodule add --depth 1 https://github.com/vagran/pulse modules/pulse
```

### Makefile

We will use CMake as the build system.

```cmake
set(CMAKE_CXX_STANDARD 20)
```
Pulse requires at least C++20 due to its use of coroutines.

```cmake
add_compile_options(-mcpu=cortex-m3 -mthumb -mfloat-abi=soft)
add_link_options(-mcpu=cortex-m3 -mthumb -mfloat-abi=soft)
```
These options configure the build for the Cortex-M3 target with Thumb instruction set support and software-based floating-point emulation.

```
add_compile_definitions(STM32F103xB)
```
This preprocessor definition selects the correct MCU model within the STM32Cube SDK.

```cmake
add_link_options(-Wl,-gc-sections,--print-memory-usage,-Map=${PROJECT_BINARY_DIR}/${PROJECT_NAME}.map)
```
These are useful linker options for embedded builds.

When combined with the `-ffunction-sections` compile option, the compiler places each function into
its own separate section. The `-gc-sections` linker flag then enables garbage collection of unused
sections, allowing the linker to strip out any functions that are not referenced in the final
binary. This typically results in a significantly smaller firmware image by removing unused code
automatically.

The remaining options provide build diagnostics:

 - `--print-memory-usage` prints a summary of memory consumption by section.
 - `-Map=...` generates a full linker map file, which is useful for debugging memory layout and
   symbol placement.

```cmake
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/src/STM32F103CBTX_FLASH.ld)
add_link_options(-T ${LINKER_SCRIPT})
```
This configuration sets a custom linker script and instructs the linker to use it during the build
process.

The linker script defines the memory layout of the firmware (Flash, RAM, and section placement),
which is essential for bare-metal embedded targets.

```cmake
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-unwind-tables>)
```
C++ exceptions and RTTI are not used in Pulse, and are generally avoided in embedded systems due to
their runtime overhead and binary size impact.

```cmake
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics -fno-use-cxa-atexit>)
```
These options disable thread-safe initialization of static variables and avoid registering
destructors with `atexit`, both of which are unnecessary in this embedded context.

```
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-c++-static-destructors>)
endif()
```
Clang provides an explicit option to disable static destructors. Since firmware execution never
exits, this cleanup code is effectively unused and can be safely omitted.

```cmake
set(PULSE_PORT ARM_CM3)
add_subdirectory("${CMAKE_SOURCE_DIR}/modules/pulse/src" "${CMAKE_BINARY_DIR}/pulse")
target_link_libraries(${PROJECT_NAME} PRIVATE pulse::pulse)
```
This is how the Pulse submodule is integrated into the project. The built-in `ARM_CM3` port is
selected to match the Cortex-M3 target used by the STM32F103.

We will also handle debug builds by disabling optimizations to make debugging easier, and by
defining a `DEBUG` preprocessor symbol that can be used in code (for example, to enable assertions).
```cmake
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -fno-omit-frame-pointer")
    add_compile_definitions(DEBUG)
endif()
```

You can now build the project using the following commands:
```bash
cmake -DCMAKE_TOOLCHAIN_FILE=arm-clang-toolchain.cmake -DCMAKE_BUILD_TYPE=Release -B build -G "Unix Makefiles"
cmake --build build
```
You may also want to use `-DCMAKE_BUILD_TYPE=MinSizeRel` to significantly reduce program memory
usage.

## Source code

The source code is located in [`src/app`](src/app).


### Linker script

See the [linker script file](src/STM32F103CBTX_FLASH.ld). It is mostly generated by STM32CubeIDE.

Pay attention to the following line:
```
_Min_Heap_Size = 0;
```
This does not reserve heap space in the way typical libc implementations do. Instead, we will define
the heap in code and register it with Pulse’s memory allocator.

```
_Min_Stack_Size = 0x400; /* required amount of stack */
```
This line specifies the amount of stack memory reserved for the application.


### Configuration files

Some components are configured through header files that define the required preprocessor symbols.


#### STM32Cube HAL configuration

The [`stm32f1xx_hal_conf.h`](src/app/stm32f1xx_hal_conf.h) file controls which HAL drivers are
enabled, along with their configuration parameters.

You should review and adjust the list of enabled modules as needed. In most cases, you will want at
least the essential peripherals such as clock control, GPIO, external interrupts, and timers. These
are enabled via definitions like:
- `HAL_RCC_MODULE_ENABLED`
- `HAL_GPIO_MODULE_ENABLED`
- `HAL_EXTI_MODULE_ENABLED`
- `HAL_TIM_MODULE_ENABLED`

Clock configuration is also defined in this file. The "Blue Pill" board uses an 8 MHz external
oscillator, so `HSE_VALUE` should be set to `8000000`.


### ETL configuration

ETL looks for an [`etl_profile.h`](src/app/etl_profile.h) file to obtain its configuration. The only
required setting in this file is `ETL_NO_STL`, which indicates that ETL is used as a completely
standalone STL replacement.


### Pulse configuration

Pulse searches for a [`pulse_config.h`](src/app/pulse_config.h) file to obtain its configuration.

The first aspect to address is memory allocation. C++ coroutines require dynamic allocation for
their coroutine frames, so this configuration is essential.

Fortunately, Pulse provides a highly optimized allocator designed for embedded systems. It can be
tuned to specific memory constraints using the following core parameters:
- `pulseConfig_MALLOC_GRANULARITY`
- `pulseConfig_MALLOC_BLOCK_SIZE_WORD_SIZE`

`pulseConfig_MALLOC_GRANULARITY` defines the smallest allocation unit and also determines alignment.
A value of `8` is generally a good default, but you may want to adjust it depending on whether RAM
is very limited or relatively abundant.

`pulseConfig_MALLOC_BLOCK_SIZE_WORD_SIZE` defines the size (in bytes) of the integer type used to
store block sizes. For example, a value of `2` means a 16-bit unsigned integer is used to represent
the number of allocation units.

With a granularity of 8 bytes and a 2-byte block size field, the maximum representable allocation
size is:
```
8 × 65536 = 524288 bytes
```

The actual maximum is slightly lower due to allocator overhead, but it is in that range. If needed,
the exact value can be queried at runtime using `pulse::GetMallocMaxSize()`.

Each allocated block also carries size metadata. In this configuration, the overhead is `2 × 2 = 4`
bytes per block.

By balancing these two parameters, you can tune the allocator for your specific constraints, trading
off between overhead and maximum allocatable block size.

Using `pulseConfig_MALLOC_BLOCK_SIZE_WORD_SIZE = 1` reduces overhead to just 2 bytes per block,
which can be useful in extremely constrained environments (for example, systems with only a few
hundred bytes of RAM). The validity of parameter combinations is checked at compile time by Pulse.

Enabling:
```cpp
#define pulseConfig_MALLOC_FAILED_PANIC 1
```
causes the system to fail fast (panic) on allocation failure instead of returning a null pointer.

Enabling:
```cpp
#define pulseConfig_MALLOC_STATS 1
```
turns on allocation statistics tracking, which can be accessed via `pulse::GetMallocStats()`.

If you need dynamic memory allocation in ISR context (for example, when using Pulse APIs that are
ISR-safe — although dynamic allocation in ISR context are generally avoided but may be required in
some cases), you must provide the `pulseConfig_MALLOC_LOCK` and `pulseConfig_MALLOC_UNLOCK` macros.

For example:
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
`MallocLock()` and `MallocUnlock()` must ensure that no interrupt can occur between the lock and
unlock operations. This can be implemented using Pulse’s critical section primitives, for example.

To use timers, you need to define the system tick frequency:
```cpp
#define pulseConfig_TICK_FREQ                       1000
```
The `pulse::Timer::Tick()` function (or its C equivalent `PulseTimerTick()`) must then be called at
this frequency.

`pulseConfig_MAX_SYSCALL_INTERRUPT_PRIORITY` defines the highest interrupt priority from which
ISR-safe Pulse functions may be called. Pulse functions must never be called from interrupts with a
higher priority than this level. The only exception is `pulse::Timer::Tick()`, which can be called
from interrupts of any priority. This is typically necessary because system tick interrupts are
often configured with a relatively high priority.

Pulse configuration includes internal validation through assertions in most critical checkpoints.
You can enable these checks by defining the `pulseConfig_ASSERT` macro. Typically, this is enabled
only in debug builds to avoid performance overhead and code size increase in release builds:
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
The macro takes a condition as its argument and triggers a panic if the condition evaluates to
false.


### System calls stubs

Libc implementations such as `newlib` require a set of basic system calls to be provided by the
application. If your project does not use libc-provided functionality, most of these system calls
can be implemented as empty stubs. They are defined in [`syscalls.c`](src/app/syscalls.c).


### MCU support package

The STM32Cube approach requires hardware configuration to be defined through a set of functions
known as the MCU Support Package (MSP). This code is located in [`src/app/msp`](src/app/msp) and is
typically generated by STM32Cube.

A typically required modification is to call `PulseTimerTick()` on each system tick:
```c
void
HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    HAL_IncTick();
    PulseTimerTick();
}
```
However, in this tutorial the ISR handler is moved into `main.cpp` and also handles the `TIM3`
interrupt, which is used for software PWM:
```cpp
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
```


### Application

Finally, we can move on to the application code itself.

The first step in a Pulse-based application is to provide a heap region for dynamic memory
allocation. We define the heap as follows:
```cpp
pulse::MallocUnit heap[PULSE_HEAP_UNITS_SIZE_KB(16)];
```

`pulse::MallocUnit` is a helper type whose size and alignment match the current memory allocator
configuration. This makes it the simplest and safest way to define heap storage.

The `PULSE_HEAP_UNITS_SIZE_*` macros are helper utilities used to express heap size in more
convenient units.

In this case, we allocate 16 KB of heap — effectively the remaining RAM after static data and stack
usage are accounted for. However, this application only requires a few hundred bytes for coroutine
frames, which would typically be allocated on the stack in a traditional implementation.

The allocator also provides a C API, which can be used from plain C code or as a standalone embedded
memory allocator.

After defining the heap buffer, it must be registered with the Pulse memory manager:
```cpp
pulse::AddHeapRegion(heap, sizeof(heap));
```
You can register multiple memory regions if needed, and this can be done at any point after the
application has started. This is useful on MCUs with multiple RAM banks, such as TCM RAM regions
found on some STM32 devices.

Here we define the synchronization functions for the memory manager, as specified earlier in
`pulse_config.h`:
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

We will also provide a `Panic()` function to handle unexpected error conditions:
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
In debug builds, the system halts in an infinite loop, allowing a debugger to attach and inspect the
state.

In release builds, the MCU performs a short delay and then resets. In a real application, you may
also want to signal the fault using an LED indicator or flush any pending debug output (for example
via UART) before triggering the reset.

Next, we define the I/O configuration:
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
The built-in LED is connected to `PC13`. The rotary encoder signals should be connected as follows:
channel `A` to `PA10`, channel `B` to `PA11`, and the ground pin to `GND`.

Initialize the HAL and system clock by calling the STM32Cube functions:
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

The LED anode is connected to `VCC`, and the cathode is connected to `PC13` through a resistor.
Because of this wiring, the pin is configured as `open-drain` and operates with an active-low logic
level. This is reflected in the control functions:
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

PWM is implemented using `TIM3`. Since `PC13` cannot be used as a hardware PWM output, we will
instead implement software PWM by toggling the LED pin inside the timer interrupt service routine.

The timer is configured to run at 1 kHz with 10-bit resolution.

Let’s examine the rotary encoder implementation, starting with how input events are captured in the
interrupt service routine:
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

Here we use the `TokenQueue` class to publish falling-edge events from the encoder lines. Each token
is a numeric event ID that is incremented with every event.

The constructor argument defines the maximum number of tokens that can be queued via `Push()` before
being consumed through `Take()` or `Peek()`. If the queue is full, additional tokens are discarded.

`Push()` is ISR-safe and executes synchronously, while `Take()` is asynchronous and can be awaited
inside a coroutine.

This makes `TokenQueue` a lightweight mechanism for forwarding parameter-less events from ISRs (or
other contexts) into coroutine logic. For events that need to carry parameters or more complex data,
`DiscardQueue` can be used instead. For coroutine-to-coroutine communication, `BlockingQueue` is the
appropriate choice.

The encoder initialization method creates two tasks, one for handling events from each encoder line:
```cpp
void
RotaryEncoder::Initialize()
{
    pulse::Task::Spawn(LineTask(true), pulse::Task::HIGHEST_PRIORITY).Pin();
    pulse::Task::Spawn(LineTask(false), pulse::Task::HIGHEST_PRIORITY).Pin();
}
```
You should use `pulse::Task::Spawn()` to create a new coroutine (task). This method registers the
task with the Pulse scheduler, and the task will be executed when the scheduler switches to the next
ready task. Tasks are scheduled according to their priority.

The handle returned by `Spawn()` (which is actually exactly the same object passed into it) is
essentially a shared pointer to the coroutine frame. The coroutine is destroyed when the last
reference to it is released. The scheduler itself only keeps references to tasks that are currently
scheduled or active. Once a task reaches a suspension point, the scheduler typically no longer holds
a direct reference to it.

Awaiters also generally do not keep strong references (they use weak references internally) to avoid
reference cycles. As a result, if nothing holds the task handle, the coroutine may be destroyed
after its first suspension point.

To prevent this, you can either:
- store the task handle for the lifetime of the task, or
- use the `Pin()` method to pin the task, preventing destruction even when the last external
  reference is released.

Awaiting a task result (by `co_await task.Wait()` or just `co_await task`) also keeps it alive by
holding a reference.

Be careful with pinning short-lived or repetitive tasks: they should either be explicitly unpinned
using `Unpin()`, or preferably managed via stored handles. `Pin()` is intended for long-lived tasks
that run for the entire application lifetime and do not need external ownership, such as in this
example where encoder line handlers run indefinitely:
```cpp
pulse::TaskV
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
            size_t idx = co_await pulse::Task::WhenAny(lineEvents, jitterTimer);
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
This is a task function, identified by its return type. `TaskV` is used for tasks that return no
result, while `TTask<TRet>` is used when a result value is produced. Both derive from the `Task`
base class, which represents a result-independent coroutine handle.

In this example, the task first waits for an event from the corresponding line’s token queue. Once a
falling edge is detected, it starts an anti-jitter (debounce) delay using a timer. The goal is to
ensure that the signal remains stable for the entire delay duration.

A `Timer` object can itself be awaited to implement delays directly within a coroutine. Simple
delays can also be performed using the static `Timer::Delay()` method, or even more conveniently by
awaiting duration literals:
```cpp
using namespace etl::chrono_literals;
using namespace pulse::duration_await;

co_await 1_s;
```

The `Task::WhenAny()` method allows waiting on multiple awaitables simultaneously. It returns the
index of the first awaiter that becomes ready. If you need to wait for all awaiters to complete, use
`Task::WhenAll()` instead. Both functions accept tasks, awaiters, or any awaitable object (i.e.
types implementing `operator co_await`).

In this code, if the line event becomes ready first, it means the signal changed again before the
debounce interval expired, so the delay is restarted. If the timer expires first, no new transitions
occurred during the delay window. Since rising edges are not explicitly handled here, the code then
checks whether the line is still in the active (low) state. If it is, the filtered event is
committed.

```cpp
// Defined in RotaryEncoder class
etl::optional<bool> lastDir;
bool lastLine = false, halfClick = false;

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
```
The rotation direction is determined by sampling the state of the opposite encoder line at the
moment a falling edge is detected.

A full encoder detent ("click") corresponds to one complete cycle, where both lines transition
through their active states in a consistent direction. This function tracks that sequence and only
emits a `CommitClick()` when a full, valid cycle has been detected.

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
Here we use a discard queue to propagate detected click events. The queue stores the accumulated
number of clicks in a given direction, with the sign indicating direction.

The base `DiscardQueue` class uses externally provided storage, which can be either dynamically or
statically allocated. The `InlineDiscardQueue` (and other types prefixed with `Inline`) embeds
fixed-size internal storage defined by a template parameter.

The resulting click stream is exposed through the class interface as follows:

```cpp
/** Get next click event. Value is number of clicks accumulated in given direction. Direction is
 * represented by sign.
 */
pulse::Awaitable<int8_t>
WaitClick()
{
    co_return co_await clicks.Pop();
}
```
You can define a coroutine by using the `pulse::Awaitable` return type. It is similar to `Task`, but
differs in its initial execution behavior.

A `Task` begins execution only when it is scheduled by the Pulse scheduler. In contrast, an
`Awaitable` starts executing immediately in a synchronous context until it reaches its first
suspension point, at which point it suspends (unless the corresponding awaiter is already ready).

The `Task::Spawn()` method accepts only `Task` types (including `TaskV` and `TTask`). In contrast,
`Awaitable` is intended for functions that are invoked directly and may later be awaited.

Here is the task that processes encoder click events:
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
            // Limit indication is in progress
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
It adjusts the current brightness value accordingly. The resulting PWM duty cycle is computed using
`CalculatePwm()`, which applies a non-linear mapping to better match the human eye’s perception of
brightness.

An additional task is spawned to indicate when the maximum brightness limit is reached. While this
indication task is active, further brightness adjustments are temporarily blocked.

The indication itself is implemented as a short LED blink:
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
`Timer::Delay()` is used here to implement simple timed delays within a coroutine.
`Task::ReleaseHandle()` clears the task handle and releases its reference to the coroutine, allowing
the system to consider the indication task finished. This, in turn, unblocks further brightness
adjustments from encoder input.

The final step is to spawn the top-level application tasks and start the Pulse scheduler from
`main()`:
```cpp
rotEnc.Initialize();

pulse::Task::Spawn(RotaryEncoderTask()).Pin();

pulse::Task::RunScheduler();

Panic("Scheduler exited");
```
This is the typical `main()` pattern for a Pulse-based application. `Task::RunScheduler()` is
expected never to return: it drives execution of all scheduled tasks according to their priorities.

If no tasks are ready to run, the scheduler places the MCU into a low-power sleep state and resumes
execution when an interrupt schedules a new task.
