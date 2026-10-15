#include "io/xml.h"
#include "base.h"

struct XNode { char *name; char **attrs; XNode **kids; };   // attrs: key, value, key, value...

static char *dup_range(const char *a, const char *b) { char *s = malloc((size_t)(b - a) + 1); memcpy(s, a, (size_t)(b - a)); s[b - a] = 0; return s; }

static void put_utf8(char **o, unsigned cp) {
    char *p = *o;
    if (cp < 0x80) *p++ = (char)cp;
    else if (cp < 0x800) { *p++ = (char)(0xC0 | (cp >> 6)); *p++ = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { *p++ = (char)(0xE0 | (cp >> 12)); *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); }
    else { *p++ = (char)(0xF0 | (cp >> 18)); *p++ = (char)(0x80 | ((cp >> 12) & 63)); *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); }
    *o = p;
}

static char *decode(const char *a, const char *b) {
    char *s = malloc((size_t)(b - a) * 4 + 1), *o = s;
    while (a < b) {
        if (*a != '&') { *o++ = *a++; continue; }
        const char *semi = memchr(a, ';', (size_t)(b - a));
        if (!semi || semi - a > 10) { *o++ = *a++; continue; }
        size_t n = (size_t)(semi - a - 1);
        const char *e = a + 1;
        if (n == 3 && !strncmp(e, "amp", 3)) *o++ = '&';
        else if (n == 2 && !strncmp(e, "lt", 2)) *o++ = '<';
        else if (n == 2 && !strncmp(e, "gt", 2)) *o++ = '>';
        else if (n == 4 && !strncmp(e, "quot", 4)) *o++ = '"';
        else if (n == 4 && !strncmp(e, "apos", 4)) *o++ = '\'';
        else if (*e == '#') put_utf8(&o, (unsigned)(e[1] == 'x' || e[1] == 'X' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10)));
        else { memcpy(o, a, (size_t)(semi - a + 1)); o += semi - a + 1; }
        a = semi + 1;
    }
    *o = 0;
    return s;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

XNode *xml_parse(const char *t, size_t len) {
    const char *p = t, *end = t + len;
    XNode *root = NULL, *stack[256]; int depth = 0;
    while (p < end) {
        if (*p != '<') { p++; continue; }
        if (p + 4 <= end && !strncmp(p, "<!--", 4)) { const char *q = strstr(p + 4, "-->"); if (!q) break; p = q + 3; continue; }
        if (p + 9 <= end && !strncmp(p, "<![CDATA[", 9)) { const char *q = strstr(p + 9, "]]>"); if (!q) break; p = q + 3; continue; }
        if (p[1] == '?' || p[1] == '!') { const char *q = memchr(p, '>', (size_t)(end - p)); if (!q) break; p = q + 1; continue; }
        if (p[1] == '/') {
            const char *q = memchr(p, '>', (size_t)(end - p)); if (!q) break;
            if (depth) depth--;
            p = q + 1;
            continue;
        }
        p++;
        const char *ns = p;
        while (p < end && !is_space(*p) && *p != '>' && *p != '/') p++;
        XNode *n = calloc(1, sizeof(XNode));
        n->name = dup_range(ns, p);
        for (;;) {
            while (p < end && is_space(*p)) p++;
            if (p >= end) break;
            if (*p == '>' || *p == '/') break;
            const char *ks = p;
            while (p < end && *p != '=' && !is_space(*p) && *p != '>') p++;
            char *key = dup_range(ks, p);
            while (p < end && is_space(*p)) p++;
            char *val = NULL;
            if (p < end && *p == '=') {
                p++;
                while (p < end && is_space(*p)) p++;
                char q = *p;
                if (q == '"' || q == '\'') {
                    const char *vs = ++p;
                    while (p < end && *p != q) p++;
                    val = decode(vs, p);
                    if (p < end) p++;
                }
            }
            if (!val) val = calloc(1, 1);
            arrput(n->attrs, key); arrput(n->attrs, val);
        }
        bool selfclose = p < end && *p == '/';
        while (p < end && *p != '>') p++;
        p++;
        if (depth) arrput(stack[depth - 1]->kids, n);
        else if (!root) root = n;
        else { xml_free(n); continue; }
        if (!selfclose && depth < 255) stack[depth++] = n;
    }
    return root;
}

void xml_free(XNode *n) {
    if (!n) return;
    for (ptrdiff_t i = 0; i < arrlen(n->kids); i++) xml_free(n->kids[i]);
    for (ptrdiff_t i = 0; i < arrlen(n->attrs); i++) free(n->attrs[i]);
    arrfree(n->kids); arrfree(n->attrs);
    free(n->name);
    free(n);
}
const char *xml_name(XNode *n) { return n ? n->name : ""; }
const char *xml_attr(XNode *n, const char *name) {
    for (ptrdiff_t i = 0; n && i + 1 < arrlen(n->attrs); i += 2) if (!strcmp(n->attrs[i], name)) return n->attrs[i + 1];
    return NULL;
}
XNode *xml_child(XNode *n, const char *name) {
    for (ptrdiff_t i = 0; n && i < arrlen(n->kids); i++) if (!strcmp(n->kids[i]->name, name)) return n->kids[i];
    return NULL;
}
int xml_nkids(XNode *n) { return n ? (int)arrlen(n->kids) : 0; }
XNode *xml_kid(XNode *n, int i) { return n->kids[i]; }
