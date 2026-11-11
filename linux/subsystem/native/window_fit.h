#pragma once
#include <stdbool.h>
#include <math.h>
#include <limits.h>

struct MatonSizeHints { int min_width,min_height,max_width,max_height; };
struct MatonFit { double scale,x,y; int width,height; };
static inline int maton_size_clamp(int size,int min,int max) {
  if(min>0&&size<min)size=min;
  /* Malformed contradictory hints: minimum wins. */
  if(max>0&&max>=min&&size>max)size=max;
  return size;
}
/* Size for an output smaller than the client's minimum. Raising one side to
 * its minimum makes maton_fit scale the window down. Grow the other side by
 * the same factor so the scaled window still fills the output instead of
 * leaving bars. */
static inline void maton_size_fill(int output_width,int output_height,struct MatonSizeHints h,int* width,int* height) {
  int w=maton_size_clamp(output_width,h.min_width,h.max_width);
  int ht=maton_size_clamp(output_height,h.min_height,h.max_height);
  if(output_width>0&&output_height>0) {
    /* Compare w/output_width with ht/output_height without floats. */
    long long a=(long long)w*output_height,b=(long long)ht*output_width;
    long long q;
    if(b>a){q=(b+output_height/2)/output_height;w=maton_size_clamp(q>INT_MAX?INT_MAX:(int)q,h.min_width,h.max_width);}
    else if(a>b){q=(a+output_width/2)/output_width;ht=maton_size_clamp(q>INT_MAX?INT_MAX:(int)q,h.min_height,h.max_height);}
  }
  *width=w;*height=ht;
}
static inline bool maton_size_fixed(struct MatonSizeHints h) {
  return h.min_width>0&&h.min_height>0&&h.min_width==h.max_width&&h.min_height==h.max_height;
}
static inline struct MatonFit maton_fit(int output_width,int output_height,int width,int height) {
  struct MatonFit f={.scale=1,.width=width,.height=height};
  if(width<=0||height<=0||output_width<=0||output_height<=0)return f;
  f.scale=fmin((double)output_width/width,(double)output_height/height);
  f.x=(output_width-width*f.scale)/2;f.y=(output_height-height*f.scale)/2;
  return f;
}
static inline int maton_fit_edge(double offset,double scale,int edge) {
  return (int)lround(offset+scale*edge);
}
static inline bool maton_fit_input(struct MatonFit f,double x,double y,double* sx,double* sy) {
  *sx=(x-f.x)/f.scale;*sy=(y-f.y)/f.scale;
  return *sx>=0&&*sy>=0&&*sx<f.width&&*sy<f.height;
}
