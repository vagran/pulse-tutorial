# Pulse tutorial

This is tutorial for [Pulse framework](https://github.com/vagran/pulse).

Let's create a simple project using Pulse framework. We will make a rotary encoder example with a
STM32F103C8T6 MCU development board ("Blue pill"). It will decode encoder signals to direction and
position change signals, and then applying them to control built-in LED brightness. It also will
suppress input lines jittering in software. Despite this MCU has dedicated hardware for this kind of
tasks, we will do it in software to demonstrate Pulse capabilities.

