// Experimental benchmark kernel. No runtime, allocation, or browser imports.
// Counts the UTF-8 bytes after replacing isolated UTF-16 surrogates with U+FFFD.
#if defined(__wasm__)
static unsigned short input[8192];
extern "C" unsigned short* url_input() { return input; }
#endif
extern "C" int fits_url(const unsigned short* text, unsigned length) {
    static_assert(sizeof(unsigned short) == 2);
    if (length > 8192) return 0;
    if (length <= 2730) return 1;
    unsigned bytes = 0;
    for (unsigned i = 0; i < length; ++i) {
        unsigned ch = text[i];
        if (ch < 0x80) ++bytes;
        else if (ch < 0x800) bytes += 2;
        else if (ch >= 0xd800 && ch <= 0xdbff && i + 1 < length && text[i+1] >= 0xdc00 && text[i+1] <= 0xdfff) {
            bytes += 4; ++i;
        } else bytes += 3;
        if (bytes > 8192) return 0;
    }
    return 1;
}
