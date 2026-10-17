#include "wsi.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    struct mv_wsi_surface s;mv_wsi_surface_init(&s,42,(VkExtent2D){640,480});
    VkSurfaceCapabilitiesKHR caps;uint32_t n=99;
    assert(!mv_wsi_support(&s,VK_QUEUE_GRAPHICS_BIT,1));
    assert(mv_wsi_capabilities(&s,&caps)==VK_ERROR_SURFACE_LOST_KHR);
    assert(mv_wsi_formats(&s,&n,0)==VK_ERROR_EXTENSION_NOT_PRESENT && n==0);
    n=99;assert(mv_wsi_modes(&n,0)==VK_ERROR_EXTENSION_NOT_PRESENT && n==0);
    VkImage image;VkDeviceMemory memory;
    assert(mv_wsi_import_ahb(0,0,(VkExtent2D){0,0},VK_FORMAT_UNDEFINED,&image,&memory)==VK_ERROR_EXTENSION_NOT_PRESENT);
    assert(!image && !memory);
    puts("PASS WSI disabled: no support, formats, modes or import");
}
