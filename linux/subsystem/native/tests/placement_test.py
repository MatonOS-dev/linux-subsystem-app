#!/usr/bin/env python3
"""Run the production X window placement fraction against edge cases."""
from pathlib import Path
import re, subprocess, tempfile, sys
native=Path(__file__).resolve().parents[1]
source=(Path(sys.argv[1]) if len(sys.argv)>1 else native/'compositor_core.c').read_text()
start=re.search(r'static int placement_frac\(',source).start()
end=source.index('{',start)+1;depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
code='#include <assert.h>\n#include <stdint.h>\n'+source[start:end]+r'''
int main(void){
 assert(placement_frac(0,283,1280)==0);            /* origin edge */
 assert(placement_frac(997,283,1280)==10000);      /* right edge toast */
 assert(placement_frac(2000,283,1280)==10000);     /* past the edge clamps */
 assert(placement_frac(-5,283,1280)==0);
 {int f=placement_frac(498,283,1280);assert(f>4990&&f<5010);} /* centred stays centred */
 assert(placement_frac(10,1280,1280)==5000);       /* no free space */
 assert(placement_frac(10,1600,1280)==5000);
}
'''
with tempfile.TemporaryDirectory(prefix='maton-place-') as tmp:
    c=Path(tmp)/'t.c';c.write_text(code);b=Path(tmp)/'t'
    subprocess.run(['cc','-Wall','-Wextra','-Werror',str(c),'-o',str(b)],check=True)
    subprocess.run([str(b)],check=True)
print('PASS X window placement fractions')
