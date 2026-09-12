.. SPDX-License-Identifier: GPL-2.0+

Failsafe LED indication
=======================

Overview
--------

The web-based failsafe (recovery) workflow in U-Boot can drive the board
LEDs to show what it is currently doing. The indication is configured
**entirely through environment variables**, so no board code or device tree
change is required: the same set of variables works on every board that
shares the LED label names.

The LED handling is owned by the failsafe component
(:file:`failsafe/led.c`); commands such as ``btnchk`` only request a *phase
change* through ``failsafe_led_set_phase()`` and never touch the LED uclass
directly.

When the feature is enabled (``CONFIG_WEBUI_FAILSAFE_LED``, default ``y``),
the following phases are indicated:

idle
    The ``btnchk`` command is waiting for a button to be held before it
    starts the web failsafe.

ready
    The web failsafe UI is running and waiting for an upload.

upgrade
    An image has been received and is being written to storage.

success
    The upgrade (and its validation) finished successfully.

fail
    The upgrade or its validation failed.

Each phase is optional: when its variable is unset or empty, U-Boot performs
no LED activity for that phase. Nothing blinks unless a blink was explicitly
requested.

Configuration
-------------

CONFIG_WEBUI_FAILSAFE_LED
    Enable LED status indication for the web failsafe. It depends on
    ``CONFIG_LED`` (the generic LED uclass and a LED driver). Boards without
    LEDs automatically disable this option, and it can also be turned off on
    boards that do not need the feedback. When the option is disabled, all
    the functions called by the failsafe code compile into empty stubs, so
    no caller needs an ``#ifdef``.

CONFIG_CMD_BTNCHK
    The command that, when a configured button is held for a few seconds,
    enters the web failsafe and shows the *idle* effect. It only depends on
    ``CONFIG_BUTTON``; it works on boards without LEDs and simply does not
    touch any LED in that case.

Environment variables
---------------------

Five phase variables and one timing variable control the effects.

failsafe_led_idle
    Effect shown while ``btnchk`` is waiting for a button (the *idle* phase
    above).

failsafe_led_ready
    Effect shown once the web failsafe UI is running and idle (the *ready*
    phase above).

failsafe_led_upgrade
    Effect shown while an image is being flashed (the *upgrade* phase above).

failsafe_led_success
    Effect shown when the upgrade and its validation succeeded (the *success*
    phase above).

failsafe_led_fail
    Effect shown when the upgrade or its validation failed (the *fail* phase
    above).

failsafe_led_period
    Frame rotation period in milliseconds. Defaults to ``250`` and is also the
    default *blink* period. The value is clamped to ``20..60000``: a too-fast
    rotation is invisible, a too-slow one looks frozen.

Legacy variables are still honoured as fallbacks when the phase-specific
variable is not set:

btnchk_led
    Used for the *idle* phase when ``failsafe_led_idle`` is not set.

failsafe_led
    Used for the *ready* phase when ``failsafe_led_ready`` is not set.

Effect syntax
-------------

An effect is a list of *frames* separated by ``;``::

    <effect> := <frame> [ ';' <frame> ] ...

    <frame>  := 'off' | 'none' | '-' | <spec> [ ',' <spec> ] ...

    <spec>   := <led-label> [ on | off | toggle | blink ] [ <period-ms> ]

The frames rotate, one after another, every ``failsafe_led_period``
milliseconds. Within a frame, the LEDs that are **not** listed are switched
off, so a simple chase is just a list of single-LED frames::

    green:power on;green:wan on;red:wan on

which switches green:power, then green:wan, then red:wan in turn.

Rules:

* A ``<state>`` that is omitted defaults to ``on`` -- except for the *idle*
  phase, where it defaults to ``blink`` (to keep the historical
  ``btnchk_led`` behaviour).

* A ``<spec>`` written as just ``<led-label> <period-ms>`` is interpreted as
  ``blink <period-ms>``.

* A frame written as ``off``, ``none`` or ``-`` switches every referenced LED
  off for that frame. This is handy for a flash between two patterns, e.g.
  ``green:power on;off``.

* LED labels are resolved through the LED uclass. Labels that do not exist on
  the board are silently ignored, so a single environment works across
  several boards that share the label names.

* If none of the labels in an effect exist on the board (or the LED subsystem
  is not ready), the engine stays idle for that phase and prints a one-time
  message per phase, e.g.::

    failsafe: no LED of 'failsafe_led_upgrade' exists on this board

* Limits: up to 8 distinct LEDs, 8 frames and 8 specs per frame.

Examples
--------

A 6-LED board (Nokia XG-040G-MD), using all LEDs::

    failsafe_led_idle=green:power on;green:wan on;red:wan on;green:wan-online on;green:usb-2 on;green:usb-1 on
    failsafe_led_ready=green:power blink 2000,green:wan-online blink 2000
    failsafe_led_upgrade=red:wan on,green:power on;red:wan on,green:wan on;red:wan on,green:wan-online on;red:wan on,green:usb-2 on;red:wan on,green:usb-1 on
    failsafe_led_success=green:power on,green:wan on,green:wan-online on,green:usb-2 on,green:usb-1 on
    failsafe_led_fail=red:wan blink 100

Here *idle* rotates over all six LEDs, *ready* blinks the power and online
LEDs slowly in sync, *upgrade* keeps the red WAN LED steady while the other
five LEDs chase, *success* lights all green LEDs, and *fail* fast-blinks the
red WAN LED.

A 2-LED board still allows distinct effects::

    failsafe_led_idle=red:wan on;green:power on
    failsafe_led_ready=green:power blink 2000
    failsafe_led_upgrade=red:wan blink 200,green:power blink 200
    failsafe_led_success=green:power on
    failsafe_led_fail=red:wan blink 100

A 7-LED board (Raisecom DR5364 / DR5374)::

    failsafe_led_idle=green:power on;red:wan on;green:wan on;green:lan on;red:wps on;green:mesh on;green:wifi on
    failsafe_led_ready=green:power blink 2000,green:wifi blink 2000
    failsafe_led_upgrade=red:wan on,green:power on;red:wan on,green:wan on;red:wan on,green:lan on;red:wan on,green:mesh on;red:wan on,green:wifi on;red:wan on,red:wps on
    failsafe_led_success=green:power on,green:wan on,green:lan on,green:mesh on,green:wifi on
    failsafe_led_fail=red:wan blink 100,red:wps blink 100

The variables can be set at runtime with ``env set`` and persisted with
``env save``; for a production build they are normally placed in the board's
``defenvs/<board>_env`` file so they are part of the default environment.
