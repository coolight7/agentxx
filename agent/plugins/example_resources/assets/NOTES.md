# Example Resources Plugin Notes

This file is injected into the system prompt via the `example_resources`
plugin's declarative `memory` section (plugin.yaml `memory:` list).

- Contributed by: example_resources v1.0.0
- Injection path: MemoryFileMiddleware (per-turn system prompt append)
- Removal: automatic on plugin unload/disable
