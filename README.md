# NPNG

A small header only png decoder. Requires linking against zlib for now.

When including the header must define this before the header only once:
```cpp
#define NPNG_IMPLEMENTATION
```

## Notes

- Only supports two PNG formats right now, RGB8 and RGBA8
- No interlacing support
- Might implement support for RGB16 and RGBA16 later
- Might also write my own zlib implementation to remove the zlib dependency
