/* Read-only adapter to the image's pinned Flatpak library. Extension selection
 * stays in flatpak_dir_find_remote_related_for_metadata(), including versions,
 * subdirectories, no-autodownload and download-if. No resolver is copied here. */
#include <flatpak.h>
#include "flatpak-dir-private.h"
#include "flatpak-ref-utils-private.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static void number(uint32_t n) { unsigned char b[]={n>>24,n>>16,n>>8,n};if(fwrite(b,1,4,stdout)!=4)_Exit(2); }
static void string(const char *s) { size_t n=strlen(s);if(n>128*1024)_Exit(2);number(n);if(fwrite(s,1,n,stdout)!=n)_Exit(2); }
/* These are the exact field rules from FlatpakTransaction::op_get_runtime_ref. */
static char *runtime_for_metadata(FlatpakDecomposed *ref,GKeyFile *metadata) {
    if(flatpak_decomposed_is_app(ref))return g_key_file_get_string(metadata,"Application","runtime",NULL);
    if(g_key_file_has_group(metadata,"Extra Data")&&!g_key_file_get_boolean(metadata,"Extra Data","NoRuntime",NULL))
        return g_key_file_get_string(metadata,"ExtensionOf","runtime",NULL);
    return NULL;
}
static gboolean has_extensions(GKeyFile *metadata) {
    g_auto(GStrv) groups=g_key_file_get_groups(metadata,NULL);
    for(guint i=0;groups[i];i++)if(g_str_has_prefix(groups[i],"Extension "))return TRUE;
    return FALSE;
}
static int local_requirements(const char *filename,const char *name) {
    g_autoptr(GError) error=NULL;
    g_autoptr(GKeyFile) metadata=g_key_file_new();
    g_autoptr(FlatpakDecomposed) ref=flatpak_decomposed_new_from_ref(name,&error);
    if(!ref||!g_key_file_load_from_file(metadata,filename,G_KEY_FILE_NONE,&error)){fprintf(stderr,"%s\n",error?error->message:"Invalid metadata");return 1;}
    g_autofree char *runtime=runtime_for_metadata(ref,metadata);
    string(runtime?runtime:"");number(has_extensions(metadata)?1:0);return fflush(stdout)?2:0;
}
int main(int argc,char **argv) {
    if(argc==4&&!strcmp(argv[1],"--requirements"))return local_requirements(argv[2],argv[3]);
    if(argc!=4&&argc!=5)return 2;
    g_autoptr(GError) error=NULL;
    g_autoptr(GFile) path=g_file_new_for_path(argv[1]);
    g_autoptr(FlatpakDir) dir=flatpak_dir_new(path,FALSE);
    g_autoptr(GKeyFile) metadata=g_key_file_new();
    g_autoptr(FlatpakDecomposed) ref=flatpak_decomposed_new_from_ref(argv[3],&error);
    g_autoptr(FlatpakRemoteState) state=NULL;
    g_autoptr(GPtrArray) related=NULL;
    g_autofree char *commit=NULL,*text=NULL,*runtime=NULL;
    if(!ref||!flatpak_dir_ensure_repo(dir,NULL,&error))goto failed;
    state=flatpak_dir_get_remote_state(dir,argv[2],FALSE,NULL,&error);
    if(!state)goto failed;
    if(argc==5) {
        if(!g_key_file_load_from_file(metadata,argv[4],G_KEY_FILE_NONE,&error))goto failed;
    } else {
        /* Commit, metadata and related refs share one Flatpak remote snapshot. */
        if(!flatpak_remote_state_lookup_ref(state,argv[3],&commit,NULL,NULL,NULL,NULL,&error)||
           !flatpak_remote_state_load_data(state,argv[3],NULL,NULL,&text,&error))goto failed;
        if(strlen(text)>128*1024||!g_key_file_load_from_data(metadata,text,-1,G_KEY_FILE_NONE,&error))goto failed;
        runtime=runtime_for_metadata(ref,metadata);
    }
    if(argc==4) {
        string(commit);string(text);string(runtime?runtime:"");number(has_extensions(metadata)?1:0);number(0);
        return fflush(stdout)?2:0;
    }
    related=flatpak_dir_find_remote_related_for_metadata(dir,state,ref,metadata,NULL,&error);
    if(!related)goto failed;
    uint32_t count=0;for(guint i=0;i<related->len;i++)if(((FlatpakRelated*)g_ptr_array_index(related,i))->download)count++;
    if(count>128)goto failed;
    number(count);
    for(guint i=0;i<related->len;i++){FlatpakRelated *r=g_ptr_array_index(related,i);if(r->download)string(flatpak_decomposed_get_ref(r->ref));}
    return fflush(stdout)?2:0;
failed:
    fprintf(stderr,"%s\n",error?error->message:"Invalid or oversized Flatpak metadata");
    return error&&g_error_matches(error,FLATPAK_ERROR,FLATPAK_ERROR_REF_NOT_FOUND)?3:1;
}
