# obs-rerenderer

Windows-only OBS plugin framework. Maintains a circular GPU frame buffer and
lets you plug in custom processors that receive the last N frames every render
cycle.

Useful for anything that needs more than one frame at a time: temporal blending,
real-time analysis, multi-frame effects, etc.

---

## requirements

- Windows 10/11 x64
- Visual Studio 2022
- OBS Studio 32.0.x installed to `C:\Program Files\obs-studio`

---

## setup

**1. clone the repo**

```
git clone https://github.com/clock/obs-rerenderer.git
```

**2. configure OBS SDK paths**

Copy `obs_sdk.props.example` to `obs_sdk.props` (gitignored — stays local):

```
copy obs_sdk.props.example obs_sdk.props
```

Edit `obs_sdk.props` and set:

- `OBS_INCLUDE` — path to OBS libobs headers (see below)
- `OBS_LIB` — path to generated import libs (see below)
- `OBS_INSTALL` — your OBS install dir (default: `C:\Program Files\obs-studio`)

**3. get the OBS headers**

Sparse-clone the OBS source at the matching tag (32.0.4):

```powershell
.\setup_obs_sdk.bat
```

This clones only the headers into `example_sources\obs-studio\libobs\`.

**4. generate import libs**

OBS ships without `.lib` files. Generate them from the installed DLLs:

```powershell
.\gen_libs.ps1
```

Output goes to `obs_sdk_libs\`. Run this again if you update OBS.

**5. build**

Open `obs-rerenderer.sln` in VS2022 and build. The post-build step copies
the DLL to your OBS plugins folder (requires VS run as administrator, or
disable the post-build step and copy manually from `out\Debug\`).

---

## project structure

```
src/
  core/
    frame_processor.hpp      pure virtual interface — implement this
    frame_buffer.hpp/.cpp    GPU ring buffer (gs_texrender_t)
    filter_base.hpp/.cpp     OBS callback bridge, drives the buffer + processor

  filters/
    example/                 reference implementation: tint filter
      example_processor.hpp/.cpp
      example_filter.hpp/.cpp

data/
  effects/
    example.effect           HLSL for the tint filter
  locale/
    en-US.ini                UI strings
```

---

## writing a custom processor

Implement `frame_processor` (six methods) and register an `obs_source_info`
that creates a `filter_base` wrapping your processor.

### 1. inherit frame_processor

```cpp
#include "core/frame_processor.hpp"

class my_processor : public frame_processor {
public:
    // constructor must be cheap — no GPU work here
    my_processor() = default;
    ~my_processor() override;

    // called when source resolution or FPS changes — load/rebuild GPU resources here
    void on_source_changed(const frame_processor_config &cfg) override;

    // called every frame on the render thread.
    // textures and timestamps are parallel arrays, oldest first.
    // timestamps[count-1] is the current frame time in nanoseconds.
    // must draw to the current OBS render target.
    // for passthrough: obs_source_draw(textures[count-1], 0, 0, width, height, false)
    void process(gs_texture_t *const *textures, const uint64_t *timestamps,
                 size_t count) override;

    // called on the UI thread when settings change — do not touch GPU resources
    void update(obs_data_t *settings) override;

    void get_properties(obs_properties_t *props) override;
    void get_defaults(obs_data_t *settings) override;

    // how many frames the ring buffer must hold for this processor
    size_t required_frame_count() const override { return 1; }
};
```

### get_defaults delegation

OBS calls `get_defaults` before any instance exists. The filter registration
delegates by constructing a temporary processor (cheap — no GPU work):

```cpp
static void get_defaults(obs_data_t *settings)
{
    my_processor{}.get_defaults(settings);
}
```

### 2. register an OBS filter

Wire up the standard OBS vtable trampolines, create a `filter_base` in
`create`, and call `obs_register_source`. See `src/filters/example/` for the
full pattern.

### 3. call register from plugin_main.cpp

```cpp
bool obs_module_load(void)
{
    register_example_filter();
    register_my_filter();
    return true;
}
```

---

## thread safety

| callback | thread | notes |
|---|---|---|
| `video_render` | render thread | only place to call `gs_*` functions |
| `update` | UI thread | set an atomic flag; act on it inside `process()` |
| `get_properties` | UI thread | read-only |

`filter_base` handles the dirty flag pattern — `on_update` sets
`rebuild_requested_`, and `on_video_render` detects it and calls
`on_source_changed` before the next `process()` call.

---

## reference

- [exeldro/obs-dynamic-delay](https://github.com/exeldro/obs-dynamic-delay) — production circular buffer + OBS filter pattern
- [OBS source: gpu-delay.c](https://github.com/obsproject/obs-studio/blob/master/plugins/obs-filters/gpu-delay.c) — minimal GPU filter reference
