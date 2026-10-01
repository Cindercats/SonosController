@README.md @config.md @src/main.cpp @platformio.ini

Read README.md — it contains the specification for this application.
Treat it as the source of truth for what the firmware should do.
Read config.md - it contains all the hardware and software configuration settings for the project.

neither README.md or config.md should be altered unless specifically requested.

Rewrite the existing src/main.cpp to implement the features described in
README.md:

- Keep the existing display init, backlight setup, and button debouncing
  logic for GPIO 35 and GPIO 0 unless README.md explicitly asks you to
  change that behavior.
- Do not modify the TFT_eSPI build_flags in platformio.ini — they are
  already correct for this board's nonstandard display pins.

If anything in README.md is ambiguous or conflicts with the current
implementation, stop and ask me before proceeding rather than guessing.

Run `pio run` when done to confirm it still compiles, and summarize what
you added or changed.