#include <assert.h>
#include <stdio.h>
#include "../src/qemu-syborg/hw/arm/syborg_gl2_precision.h"

int main(int argc, char **argv)
{
    char s[] = "// precision mediump float;\nprecision /*keep*/ mediump\nfloat;\n"
               "precision mediump int; mediump vec3 explicit_value;\n"
               "precision highp float; precision lowp float;\n"
               "/* precision mediump float; */ precision mediump sampler2D;";
    assert(gl2_promote_float_defaults(s, sizeof(s) - 1) == 1);
    assert(strstr(s, "precision /*keep*/ highp  \nfloat;"));
    assert(strstr(s, "precision mediump int; mediump vec3 explicit_value;"));
    assert(strstr(s, "// precision mediump float;"));
    assert(gl2_promote_float_defaults(s, sizeof(s) - 1) == 0);
    char truncated[] = "precision mediump float";
    assert(gl2_promote_float_defaults(truncated, sizeof(truncated) - 1) == 0);
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        char source[65536];
        assert(f);
        size_t n = fread(source, 1, sizeof(source), f);
        assert(!ferror(f) && n < sizeof(source));
        fclose(f);
        assert(gl2_promote_float_defaults(source, n) == 1);
        assert(gl2_promote_float_defaults(source, n) == 0);
    }
    puts("precision promotion: passed");
}
