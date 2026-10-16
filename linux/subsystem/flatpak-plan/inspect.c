/* Read-only adapter using only Flatpak's public API. */
#include <flatpak.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static void number(uint32_t n) { unsigned char b[]={n>>24,n>>16,n>>8,n};if(fwrite(b,1,4,stdout)!=4)_Exit(2); }
static void string(const char *s) { size_t n=strlen(s);if(n>128*1024)_Exit(2);number(n);if(fwrite(s,1,n,stdout)!=n)_Exit(2); }
static void related_string(FlatpakRelatedRef *item) {
    g_autofree char *ref=flatpak_ref_format_ref((FlatpakRef*)item);
    const char * const *subpaths=flatpak_related_ref_get_subpaths(item);
    if(!subpaths||!subpaths[0]){string(ref);return;}
    GString *value=g_string_new(ref);
    for(guint i=0;subpaths[i];i++){g_string_append_c(value,'\n');g_string_append(value,subpaths[i]);}
    string(value->str);g_string_free(value,TRUE);
}

static char *runtime_for_metadata(FlatpakRef *ref,GKeyFile *metadata) {
    if(flatpak_ref_get_kind(ref)==FLATPAK_REF_KIND_APP)return g_key_file_get_string(metadata,"Application","runtime",NULL);
    if(g_key_file_has_group(metadata,"Extra Data")&&!g_key_file_get_boolean(metadata,"Extra Data","NoRuntime",NULL))
        return g_key_file_get_string(metadata,"ExtensionOf","runtime",NULL);
    return NULL;
}
static gboolean has_extensions(GKeyFile *metadata) {
    g_auto(GStrv) groups=g_key_file_get_groups(metadata,NULL);
    for(guint i=0;groups[i];i++)if(g_str_has_prefix(groups[i],"Extension "))return TRUE;
    return FALSE;
}
static FlatpakRef *parse_ref(const char *name,GError **error) {
    return flatpak_ref_parse(name,error);
}
static int local_requirements(const char *filename,const char *name) {
    g_autoptr(GError) error=NULL;
    g_autoptr(GKeyFile) metadata=g_key_file_new();
    g_autoptr(FlatpakRef) ref=parse_ref(name,&error);
    if(!ref||!g_key_file_load_from_file(metadata,filename,G_KEY_FILE_NONE,&error)){fprintf(stderr,"%s\n",error?error->message:"Invalid metadata");return 1;}
    g_autofree char *runtime=runtime_for_metadata(ref,metadata);
    string(runtime?runtime:"");number(has_extensions(metadata)?1:0);return fflush(stdout)?2:0;
}
int main(int argc,char **argv) {
    if(argc==4&&!strcmp(argv[1],"--requirements"))return local_requirements(argv[2],argv[3]);
    if(argc!=4&&argc!=5)return 2;
    g_autoptr(GError) error=NULL;
    g_autoptr(GFile) path=g_file_new_for_path(argv[1]);
    g_autoptr(FlatpakInstallation) installation=flatpak_installation_new_for_path(path,FALSE,NULL,&error);
    g_autoptr(FlatpakRef) ref=parse_ref(argv[3],&error);
    g_autoptr(GKeyFile) metadata=g_key_file_new();
    g_autofree char *text=NULL,*runtime=NULL,*commit=NULL;
    g_autoptr(GPtrArray) related=NULL;
    if(!installation||!ref)goto failed;
    if(argc==5) {
        if(!g_key_file_load_from_file(metadata,argv[4],G_KEY_FILE_NONE,&error))goto failed;
    } else {
        g_autoptr(FlatpakRemoteRef) remote_ref=flatpak_installation_fetch_remote_ref_sync(installation,argv[2],flatpak_ref_get_kind(ref),flatpak_ref_get_name(ref),flatpak_ref_get_arch(ref),flatpak_ref_get_branch(ref),NULL,&error);
        g_autoptr(GBytes) bytes=flatpak_installation_fetch_remote_metadata_sync(installation,argv[2],ref,NULL,&error);
        if(!remote_ref||!bytes)goto failed;
        gsize length=0;const char *data=g_bytes_get_data(bytes,&length);
        if(length>128*1024||!g_key_file_load_from_data(metadata,data,length,G_KEY_FILE_NONE,&error))goto failed;
        text=g_strndup(data,length);runtime=runtime_for_metadata(ref,metadata);
        commit=g_strdup(flatpak_ref_get_commit((FlatpakRef*)remote_ref));
    }
    if(argc==4) {
        string(commit?commit:"");string(text?text:"");
        string(runtime?runtime:"");number(has_extensions(metadata)?1:0);number(0);
        return fflush(stdout)?2:0;
    }
    related=flatpak_installation_list_remote_related_refs_sync(installation,argv[2],argv[3],NULL,&error);
    if(!related)goto failed;
    uint32_t count=0;for(guint i=0;i<related->len;i++)if(flatpak_related_ref_should_download(g_ptr_array_index(related,i)))count++;
    if(count>128)goto failed;
    number(count);
    for(guint i=0;i<related->len;i++) {
        FlatpakRelatedRef *item=g_ptr_array_index(related,i);
        if(flatpak_related_ref_should_download(item))related_string(item);
    }
    return fflush(stdout)?2:0;
failed:
    fprintf(stderr,"%s\n",error?error->message:"Invalid or oversized Flatpak metadata");
    return error&&g_error_matches(error,FLATPAK_ERROR,FLATPAK_ERROR_REF_NOT_FOUND)?3:1;
}
