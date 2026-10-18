/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real CreateDevice retry helper; a failing lower chain consumes LINK_INFO. */
#define _GNU_SOURCE
#include "../src/layer.c"
#include <assert.h>
static VkLayerDeviceCreateInfo loader;
static VkLayerDeviceLink lower[2];
static VkDeviceCreateInfo original, patched;
static unsigned calls;
static int fail_first, fail_all;
static VKAPI_ATTR VkResult VKAPI_CALL lower_create(VkPhysicalDevice physical,
        const VkDeviceCreateInfo *info,const VkAllocationCallbacks *alloc,VkDevice *out) {
    (void)physical;(void)alloc;
    assert(loader.u.pLayerInfo==&lower[0]);
    /* Both downstream layers advance the same loader-owned cursor. */
    loader.u.pLayerInfo=loader.u.pLayerInfo->pNext;
    assert(loader.u.pLayerInfo==&lower[1]);
    loader.u.pLayerInfo=loader.u.pLayerInfo->pNext;
    assert(!loader.u.pLayerInfo);
    assert(info==(calls==0 && fail_first?&patched:&original));
    ++calls;
    if(fail_all || (fail_first && calls==1))return VK_ERROR_EXTENSION_NOT_PRESENT;
    *out=(VkDevice)(uintptr_t)42;return VK_SUCCESS;
}
static void reset(void) {
    calls=0;loader.u.pLayerInfo=&lower[0];lower[0].pNext=&lower[1];lower[1].pNext=NULL;
}
int main(void) {
    original=(VkDeviceCreateInfo){.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&loader};
    patched=original;VkDevice out=VK_NULL_HANDLE;int fallback=0;
    fail_first=1;reset();
    assert(native_create_device_with_fallback(lower_create,VK_NULL_HANDLE,&original,&patched,&loader,NULL,&out,&fallback)==VK_SUCCESS);
    assert(calls==2 && fallback && out==(VkDevice)(uintptr_t)42);
    assert(original.pNext==&loader && !original.enabledExtensionCount);
    fail_first=0;reset();
    assert(native_create_device_with_fallback(lower_create,VK_NULL_HANDLE,&original,NULL,&loader,NULL,&out,&fallback)==VK_SUCCESS);
    assert(calls==1 && !fallback);
    fail_first=fail_all=1;reset();
    assert(native_create_device_with_fallback(lower_create,VK_NULL_HANDLE,&original,&patched,&loader,NULL,&out,&fallback)==VK_ERROR_EXTENSION_NOT_PRESENT);
    assert(calls==2 && fallback);
    puts("device-fallback: consumed two-layer LINK_INFO restored, original device retried, original error preserved PASS");
}
