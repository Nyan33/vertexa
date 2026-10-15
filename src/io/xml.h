// Small tolerant XML DOM parser (enough for XFL): elements, attributes, entities, CDATA, comments.
#pragma once
#include <stddef.h>

typedef struct XNode XNode;
XNode *xml_parse(const char *text, size_t len);
void xml_free(XNode *n);
const char *xml_name(XNode *n);
const char *xml_attr(XNode *n, const char *name);
XNode *xml_child(XNode *n, const char *name);
int xml_nkids(XNode *n);
XNode *xml_kid(XNode *n, int i);
