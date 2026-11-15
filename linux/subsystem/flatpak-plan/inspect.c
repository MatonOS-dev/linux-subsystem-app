/* Read-only adapter using only Flatpak's public API. */
#include <flatpak.h>
#include <appstream.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static void number(uint32_t n) { unsigned char b[]={n>>24,n>>16,n>>8,n};if(fwrite(b,1,4,stdout)!=4)_Exit(2); }
static void string(const char *s) { size_t n=strlen(s);if(n>UINT32_MAX)_Exit(2);number((uint32_t)n);if(fwrite(s,1,n,stdout)!=n)_Exit(2); }
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
static char *parent_for_metadata(FlatpakRef *ref,GKeyFile *metadata) {
    g_autofree char *parent=g_key_file_get_string(metadata,"ExtensionOf","ref",NULL);
    /* Freedesktop's i386 compatibility extensions (Compat.i386, GL32,
     * codecs_extra.i386, and related drivers) are x86_64 refs. Their metadata
     * can name the i386 ABI runtime as ExtensionOf.ref, but Flathub does not
     * publish that runtime ref. Flatpak resolves the extension point through
     * the app's declared x86_64 runtime; do not turn this metadata relation
     * into a dependency pull. */
    if(parent&&flatpak_ref_get_kind(ref)==FLATPAK_REF_KIND_RUNTIME&&
       !strcmp(flatpak_ref_get_arch(ref),"x86_64")&&
       g_str_has_prefix(parent,"runtime/org.freedesktop.Platform/i386/"))
        return NULL;
    return g_steal_pointer(&parent);
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
    g_autofree char *parent=parent_for_metadata(ref,metadata);
    string(runtime?runtime:"");string(parent?parent:"");number(has_extensions(metadata)?1:0);return fflush(stdout)?2:0;
}
/* AppStream owns OARS mappings; do not duplicate its territory/age tables in Java. */
static int oars(int argc, char **argv) {
    if(argc<4){fprintf(stderr,"flatpak-source OARS: expected kind and locale\n");return 2;}
    g_autoptr(AsContentRating) rating=as_content_rating_new();
    as_content_rating_set_kind(rating,argv[2]);
    for(int i=4;i<argc;i++) {
        char *equals=strchr(argv[i],'=');
        if(!equals){fprintf(stderr,"flatpak-source OARS: malformed attribute %s\n",argv[i]);return 2;}
        g_autofree char *id=g_strndup(argv[i],equals-argv[i]);
        AsContentRatingValue value=as_content_rating_value_from_string(equals+1);
        if(value==AS_CONTENT_RATING_VALUE_UNKNOWN){fprintf(stderr,"flatpak-source OARS: unknown level %s\n",equals+1);return 2;}
        as_content_rating_set_value(rating,id,value);
    }
    guint age=as_content_rating_get_minimum_age(rating);
    if(age==G_MAXUINT){fprintf(stderr,"flatpak-source OARS: unknown rating kind %s\n",argv[2]);return 1;}
    AsContentRatingSystem system=as_content_rating_system_from_locale(argv[3]);
    g_autofree char *label=as_content_rating_system_format_age(system,age);
    printf("%u\n%s\n%s\n",age,as_content_rating_system_to_string(system),label?label:"");
    if(fflush(stdout)){perror("flatpak-source OARS stdout");return 1;}
    return 0;
}
static int installed_refs(const char *directory, gboolean updates) {
    g_autoptr(GError) error=NULL;
    g_autoptr(GFile) path=g_file_new_for_path(directory);
    g_autoptr(FlatpakInstallation) installation=flatpak_installation_new_for_path(path,FALSE,NULL,&error);
    g_autoptr(GPtrArray) refs=NULL;
    if(!installation)goto failed;
    refs=updates?flatpak_installation_list_installed_refs_for_update(installation,NULL,&error):
        flatpak_installation_list_installed_refs(installation,NULL,&error);
    if(!refs)goto failed;
    for(guint i=0;i<refs->len;i++) {
        FlatpakInstalledRef *ref=g_ptr_array_index(refs,i);
        g_autofree char *name=flatpak_ref_format_ref((FlatpakRef*)ref);
        printf("%s\t%s\n",name,flatpak_installed_ref_get_origin(ref));
    }
    if(fflush(stdout)){perror("flatpak-source installed stdout");return 1;}
    return 0;
failed:
    fprintf(stderr,"flatpak-source installed directory=%s: %s\n",directory,error?error->message:"Invalid installation");return 1;
}
static int sizes(char **argv) {
    g_autoptr(GError) error=NULL;
    g_autoptr(GFile) path=g_file_new_for_path(argv[2]);
    g_autoptr(FlatpakInstallation) installation=flatpak_installation_new_for_path(path,FALSE,NULL,&error);
    g_autoptr(FlatpakRef) ref=NULL;
    g_autoptr(FlatpakRemoteRef) remote=NULL;
    if(!installation)goto failed;
    ref=flatpak_ref_parse(argv[4],&error);
    if(!ref)goto failed;
    remote=flatpak_installation_fetch_remote_ref_sync(installation,argv[3],
        flatpak_ref_get_kind(ref),flatpak_ref_get_name(ref),flatpak_ref_get_arch(ref),flatpak_ref_get_branch(ref),NULL,&error);
    if(!remote)goto failed;
    printf("%" G_GUINT64_FORMAT "\n%" G_GUINT64_FORMAT "\n",
        flatpak_remote_ref_get_download_size(remote),flatpak_remote_ref_get_installed_size(remote));
    if(fflush(stdout)){perror("flatpak-source sizes stdout");return 1;}
    return 0;
failed:
    fprintf(stderr,"flatpak-source sizes remote=%s ref=%s: %s\n",argv[3],argv[4],error?error->message:"Invalid ref");return 1;
}
int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"--installed"))return installed_refs(argv[2],FALSE);
    if(argc==3&&!strcmp(argv[1],"--updates"))return installed_refs(argv[2],TRUE);
    if(argc==5&&!strcmp(argv[1],"--sizes"))return sizes(argv);
    if(argc>=2&&!strcmp(argv[1],"--oars"))return oars(argc,argv);
    if(argc==4&&!strcmp(argv[1],"--requirements"))return local_requirements(argv[2],argv[3]);
    if(argc!=4&&argc!=5)return 2;
    g_autoptr(GError) error=NULL;
    g_autoptr(GFile) path=g_file_new_for_path(argv[1]);
    g_autoptr(FlatpakInstallation) installation=flatpak_installation_new_for_path(path,FALSE,NULL,&error);
    g_autoptr(FlatpakRef) ref=parse_ref(argv[3],&error);
    g_autoptr(GKeyFile) metadata=g_key_file_new();
    g_autofree char *text=NULL,*runtime=NULL,*parent=NULL,*commit=NULL;
    g_autoptr(GPtrArray) related=NULL;
    if(!installation||!ref)goto failed;
    if(argc==5) {
        if(!g_key_file_load_from_file(metadata,argv[4],G_KEY_FILE_NONE,&error))goto failed;
    } else {
        g_autoptr(FlatpakRemoteRef) remote_ref=flatpak_installation_fetch_remote_ref_sync(installation,argv[2],flatpak_ref_get_kind(ref),flatpak_ref_get_name(ref),flatpak_ref_get_arch(ref),flatpak_ref_get_branch(ref),NULL,&error);
        g_autoptr(GBytes) bytes=flatpak_installation_fetch_remote_metadata_sync(installation,argv[2],ref,NULL,&error);
        if(!remote_ref||!bytes)goto failed;
        gsize length=0;const char *data=g_bytes_get_data(bytes,&length);
        if(!g_key_file_load_from_data(metadata,data,length,G_KEY_FILE_NONE,&error))goto failed;
        text=g_strndup(data,length);runtime=runtime_for_metadata(ref,metadata);parent=parent_for_metadata(ref,metadata);
        commit=g_strdup(flatpak_ref_get_commit((FlatpakRef*)remote_ref));
    }
    if(argc==4) {
        string(commit?commit:"");string(text?text:"");
        string(runtime?runtime:"");string(parent?parent:"");number(has_extensions(metadata)?1:0);number(0);
        return fflush(stdout)?2:0;
    }
    /* The transaction's scratch installation has fetched remote metadata but
     * may not yet have the OSTree summary that Flatpak's related-ref resolver
     * reads. Refresh through Flatpak first so extension refs (including
     * Compat.i386 selected from Steam's extension point) come from its own
     * resolver, not a locally synthesized ref. */
    if(!flatpak_installation_update_remote_sync(installation,argv[2],NULL,&error))goto failed;
    related=flatpak_installation_list_remote_related_refs_sync(installation,argv[2],argv[3],NULL,&error);
    if(!related)goto failed;
    uint32_t count=0;for(guint i=0;i<related->len;i++)if(flatpak_related_ref_should_download(g_ptr_array_index(related,i))) {
        if(count==UINT32_MAX)goto failed;
        count++;
    }
    number(count);
    for(guint i=0;i<related->len;i++) {
        FlatpakRelatedRef *item=g_ptr_array_index(related,i);
        if(flatpak_related_ref_should_download(item))related_string(item);
    }
    return fflush(stdout)?2:0;
failed:
    fprintf(stderr,"%s\n",error?error->message:"Invalid Flatpak metadata");
    return error&&g_error_matches(error,FLATPAK_ERROR,FLATPAK_ERROR_REF_NOT_FOUND)?3:1;
}
