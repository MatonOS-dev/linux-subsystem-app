#include "../window_fit.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
int main(void) {
  assert(maton_size_clamp(800,0,0)==800);
  assert(maton_size_clamp(800,900,0)==900);
  assert(maton_size_clamp(800,0,600)==600);
  assert(maton_size_clamp(800,900,600)==900);
  assert(maton_size_clamp(INT_MAX,0,0)==INT_MAX);
  assert(maton_size_fixed((struct MatonSizeHints){640,480,640,480}));
  assert(!maton_size_fixed((struct MatonSizeHints){0}));
  assert(!maton_size_fixed((struct MatonSizeHints){640,0,640,480}));
  struct MatonFit f=maton_fit(1000,800,640,480);
  assert(f.scale==1.5625&&f.x==0&&f.y==25);
  double x,y;
  assert(maton_fit_input(f,500,400,&x,&y)&&x==320&&y==240);
  assert(!maton_fit_input(f,500,24,&x,&y));
  assert(!maton_fit_input(f,500,775,&x,&y));
  assert(maton_fit_input(f,0,25,&x,&y)&&x==0&&y==0);
  assert(!maton_fit_input(f,1000,25,&x,&y));
  f=maton_fit(800,800,400,800);
  assert(f.x==200&&f.y==0&&f.scale==1);
  assert(!maton_fit_input(f,199,400,&x,&y));
  assert(!maton_fit_input(f,600,400,&x,&y));
  f=maton_fit(320,200,640,480);
  assert(f.scale==200.0/480&&f.y==0);
  assert(maton_fit_edge(f.x,f.scale,640)==293);
  assert(maton_fit_edge(f.x,f.scale,0)==27);
  /* Adjacent children share one rounded edge; input retains subpixel precision. */
  assert(maton_fit_edge(f.x,f.scale,13)==(int)lround(f.x+13*f.scale));
  assert(maton_fit_input(f,f.x+13.25*f.scale,10*f.scale,&x,&y));
  assert(fabs(x-13.25)<1e-10&&fabs(y-10)<1e-10);
  f=maton_fit(640,480,640,480);
  assert(f.scale==1&&f.x==0&&f.y==0);
  puts("window_fit_test: passed");
}
