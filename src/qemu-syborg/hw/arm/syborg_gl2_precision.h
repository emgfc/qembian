/* Promote default float precision without moving source lines or touching comments.
 * GLES precision qualifiers specify minimums; this compatibility policy supplies
 * highp defaults in both shader stages so shared uniforms keep matching qualifiers.
 * The caller owns the writable, explicitly sized source (no terminating NUL needed).
 */
#ifndef SYBORG_GL2_PRECISION_H
#define SYBORG_GL2_PRECISION_H
#include <stddef.h>
#include <string.h>

static int gl2_precision_ident(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

static int gl2_promote_float_defaults(char *src, size_t len)
{
    size_t at = 0, qualifier = 0, qualifier_len = 0;
    int state = 0, changed = 0;
    while (at < len) {
        unsigned char ch = src[at];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { at++; continue; }
        if (ch == '/' && at + 1 < len && src[at + 1] == '/') {
            at += 2;
            while (at < len && src[at] != '\n') at++;
            continue;
        }
        if (ch == '/' && at + 1 < len && src[at + 1] == '*') {
            at += 2;
            while (at + 1 < len && !(src[at] == '*' && src[at + 1] == '/')) at++;
            at = at + 1 < len ? at + 2 : len;
            continue;
        }
        size_t start = at++;
        if (gl2_precision_ident(ch)) {
            while (at < len && gl2_precision_ident(src[at])) at++;
        }
        size_t n = at - start;
        if (n == 9 && !memcmp(src + start, "precision", 9)) {
            state = 1;
        } else if (state == 1 && n == 7 && !memcmp(src + start, "mediump", 7)) {
            qualifier = start;
            qualifier_len = n;
            state = 2;
        } else if (state == 2 && n == 5 && !memcmp(src + start, "float", 5)) {
            state = 3;
        } else {
            if (state == 3 && ch == ';' && qualifier_len == 7) {
                memcpy(src + qualifier, "highp  ", 7);
                changed++;
            }
            state = 0;
        }
    }
    return changed;
}
#endif
