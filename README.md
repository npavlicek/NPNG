# NPNG

A small header only png decoder. Requires linking against zlib for now.

When including the header must define this before the header only once:
```cpp
#define NPNG_IMPLEMENTATION
```
## To-do

- [ ] Implement crc32 and adler32
- [ ] Error checking for my zlib implementation

## Notes

- Only supports two PNG formats right now, RGB8 and RGBA8
- Might implement support for RGB16 and RGBA16 later
- No interlacing support
- No color palette support
- Only PNG block types that are supported are IHDR, IDAT, IEND
