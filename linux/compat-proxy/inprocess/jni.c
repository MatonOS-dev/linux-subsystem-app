/* SPDX-License-Identifier: Apache-2.0 */
#include "routing.h"
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
static void release(char **values) {
    if(!values)return;
    for(unsigned i=0;values[i];i++)free(values[i]);
    free(values);
}
static char **strings(JNIEnv *e,jobjectArray array) {
    if(!array)return NULL;
    jsize count=(*e)->GetArrayLength(e,array);
    if(count>4096)return NULL;
    char **out=calloc((size_t)count+1,sizeof(*out));
    if(!out)return NULL;
    for(jsize i=0;i<count;i++) {
        jstring item=(*e)->GetObjectArrayElement(e,array,i);
        if(!item){release(out);return NULL;}
        const char *s=(*e)->GetStringUTFChars(e,item,NULL);
        if(!s){(*e)->DeleteLocalRef(e,item);release(out);return NULL;}
        out[i]=strdup(s);
        (*e)->ReleaseStringUTFChars(e,item,s);(*e)->DeleteLocalRef(e,item);
        if(!out[i]){release(out);return NULL;}
    }
    return out;
}
JNIEXPORT jint JNICALL Java_org_matonos_linux_stub_InProcessLauncher_nativeRunInProcess(
        JNIEnv *e,jclass cls,jobjectArray args,jobjectArray environment,jintArray descriptors) {
    (void)cls;
    if(!descriptors || (*e)->GetArrayLength(e,descriptors)!=2)return -EINVAL;
    char **argv=strings(e,args),**env=strings(e,environment);
    if(!argv || !argv[0] || !env){release(argv);release(env);return -EINVAL;}
    jint source[2];(*e)->GetIntArrayRegion(e,descriptors,0,2,source);
    int fds[]={dup(source[0]),dup(source[1])};
    int status=fds[0]<0 || fds[1]<0?-EBADF:inprocess_run_fds(argv,env,fds);
    for(unsigned i=0;i<2;i++)if(fds[i]>=0)close(fds[i]);
    release(argv);release(env);return status;
}
