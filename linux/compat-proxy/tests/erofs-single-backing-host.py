#!/usr/bin/env python3
"""Model bwrap opening a physical deployment files/ prefix backed by EROFS."""
import pathlib
import shutil
import subprocess

base=pathlib.Path(__file__).resolve().parents[1]
root=base/'build/erofs-single-backing'
shutil.rmtree(root,ignore_errors=True)
deployment=root/'deployment/runtime/org.freedesktop.Platform/x86_64/25.08/commit/files'
deployment.mkdir(parents=True)
image=root/'image/25.08'
(image/'lib/x86_64-linux-gnu/GL/default').mkdir(parents=True)
(image/'.image-marker').write_text('image')
newroot=root/'newroot'
newroot.mkdir(parents=True)
source=root/'probe.c'
source.write_text(r'''#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#c,errno);return 1;} } while(0)
int main(int argc,char **argv){
 CHECK(argc==2);
 /* This is the path bwrap opens before mkdir-parents traverses descendants. */
 int usr=open(argv[1],O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(usr>=0);
 struct stat st;
 CHECK(fstatat(usr,".image-marker",&st,0)==0 && S_ISREG(st.st_mode));
 CHECK(fstatat(usr,"lib",&st,0)==0 && S_ISDIR(st.st_mode));
 CHECK(mkdirat(usr,"lib",0755)==-1 && errno==EEXIST);
 int gl=openat(usr,"lib/x86_64-linux-gnu/GL/default",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
 CHECK(gl>=0);close(gl);close(usr);
 puts("PASS physical deployment files/ prefix opens the matching image; bwrap stat/mkdirat sees existing EROFS dirs");
 return 0;
}''')
probe=root/'probe'
subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror',str(source),'-o',str(probe)],check=True)
# These are the arguments emitted by ErofsLaunch and DeclaredPackages:
# --erofs-mount <target> <mount>/<branch>, then --erofs-backing <target> <files/>.
cmd=[str(base/'build/linux-layer-v3'),'--android-filter',
     '--map-ro',str(deployment),str(newroot/'usr'),
     '--erofs-mount','/usr',str(image),
     '--erofs-backing','/usr',str(deployment),
     str(probe),str(newroot/'usr')]
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print(result.stdout.strip())
