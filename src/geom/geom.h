// Geometry helpers shared by the tools: hit tests, intersections, simplification, contour tracing.
#pragma once
#include "doc/doc.h"

float dist_point_seg(V2 p, V2 a, V2 b, float *t);
bool seg_isect(V2 a, V2 b, V2 c, V2 d, float *t, float *u);   // proper intersection of ab and cd
bool point_in_poly(const V2 *poly, const int *starts, int npoly, int nstarts, V2 p);   // even-odd over all contours

// polyline of a stroke's centerline (dense) with cumulative arc length; caller frees with arrfree
typedef struct { SPt *p; float *s; float len; } Path;
Path path_of(const Item *st, float step);
void path_free(Path *p);
SPt path_at(const Path *p, float s);        // point at arc length s (interpolated)
float path_project(const Path *p, V2 q, float *dist);   // arc length of the closest point

bool item_hit(const Item *it, V2 p, float tol);         // strokes (within radius + tol) and fills
Item stroke_from_dense(const SPt *pts, int n, const Item *style, float eps);   // simplified copy keeping style

SPt *simplify_spt(const SPt *p, int n, float eps);      // Douglas-Peucker over (x, y, r)
V2 *simplify_v2(const V2 *p, int n, float eps, bool closed);

// binary mask -> closed contours along pixel edges (even-odd), in mask pixel units
void trace_mask(const u8 *mask, int w, int h, V2 **poly, int **starts);
