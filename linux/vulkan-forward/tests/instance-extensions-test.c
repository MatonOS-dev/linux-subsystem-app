/* Test enumeration before libwayland is loaded, as dlopen-based WSI clients do. */
#include "../icd.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint32_t count=0;
    assert(mv_wsi_instance_extensions(&count,0)==VK_SUCCESS && count==2);
    VkExtensionProperties props[2];
    count=1;
    assert(mv_wsi_instance_extensions(&count,props)==VK_INCOMPLETE && count==1);
    assert(!strcmp(props[0].extensionName,VK_KHR_SURFACE_EXTENSION_NAME));
    count=2;
    assert(mv_wsi_instance_extensions(&count,props)==VK_SUCCESS && count==2);
    assert(!strcmp(props[1].extensionName,VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME));
    assert(mv_wsi_instance_extension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME));
    assert(!mv_wsi_instance_extension("VK_KHR_xcb_surface"));
    puts("PASS WSI extension discovery before Wayland dlopen");
}
