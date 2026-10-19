/* SPDX-License-Identifier: GPL-3.0-or-later

 * WSI query/release calls share the output/monitor swapchain host lock.

 * No timestamps, IDs or caller structures are modified. */

static pthread_mutex_t *native_query_lock(native_device *d, VkSwapchainKHR sc) {
    gfg_output *output = native_find_output(d,sc);
    if (output) return gfg_output_wsi(output);
    native_present_ids *ids = native_find_ids(d,sc);
    return ids ? &ids->wsi_lock : NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL native_GetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    if(native_test_find_virtual(d,swapchain))return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    PFN_vkGetSwapchainStatusKHR next = (PFN_vkGetSwapchainStatusKHR)d->gdpa(device,"vkGetSwapchainStatusKHR");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    pthread_mutex_t *lock = native_query_lock(d,swapchain);
    if (lock) pthread_mutex_lock(lock);
    VkResult result = next(device, swapchain);
    if (lock) pthread_mutex_unlock(lock);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL native_GetRefreshCycleDurationGOOGLE(VkDevice device, VkSwapchainKHR swapchain, VkRefreshCycleDurationGOOGLE* pDisplayTimingProperties) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    if(native_test_find_virtual(d,swapchain))return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    PFN_vkGetRefreshCycleDurationGOOGLE next = (PFN_vkGetRefreshCycleDurationGOOGLE)d->gdpa(device,"vkGetRefreshCycleDurationGOOGLE");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    pthread_mutex_t *lock = native_query_lock(d,swapchain);
    if (lock) pthread_mutex_lock(lock);
    VkResult result = next(device, swapchain, pDisplayTimingProperties);
    if (lock) pthread_mutex_unlock(lock);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL native_GetPastPresentationTimingGOOGLE(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pPresentationTimingCount, VkPastPresentationTimingGOOGLE* pPresentationTimings) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    if(native_test_find_virtual(d,swapchain))return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    PFN_vkGetPastPresentationTimingGOOGLE next = (PFN_vkGetPastPresentationTimingGOOGLE)d->gdpa(device,"vkGetPastPresentationTimingGOOGLE");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    pthread_mutex_t *lock = native_query_lock(d,swapchain);
    if (lock) pthread_mutex_lock(lock);
    VkResult result = next(device, swapchain, pPresentationTimingCount, pPresentationTimings);
    if (lock) pthread_mutex_unlock(lock);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL native_SetSwapchainPresentTimingQueueSizeEXT(VkDevice device, VkSwapchainKHR swapchain, uint32_t size) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    if(native_test_find_virtual(d,swapchain))return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    PFN_vkSetSwapchainPresentTimingQueueSizeEXT next = (PFN_vkSetSwapchainPresentTimingQueueSizeEXT)d->gdpa(device,"vkSetSwapchainPresentTimingQueueSizeEXT");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    pthread_mutex_t *lock = native_query_lock(d,swapchain);
    if (lock) pthread_mutex_lock(lock);
    VkResult result = next(device, swapchain, size);
    if (lock) pthread_mutex_unlock(lock);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL native_GetSwapchainTimingPropertiesEXT(VkDevice device, VkSwapchainKHR swapchain, VkSwapchainTimingPropertiesEXT* pSwapchainTimingProperties, uint64_t* pSwapchainTimingPropertiesCounter) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    if(native_test_find_virtual(d,swapchain))return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    PFN_vkGetSwapchainTimingPropertiesEXT next = (PFN_vkGetSwapchainTimingPropertiesEXT)d->gdpa(device,"vkGetSwapchainTimingPropertiesEXT");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    pthread_mutex_t *lock = native_query_lock(d,swapchain);
    if (lock) pthread_mutex_lock(lock);
    VkResult result = next(device, swapchain, pSwapchainTimingProperties, pSwapchainTimingPropertiesCounter);
    if (lock) pthread_mutex_unlock(lock);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL native_GetSwapchainTimeDomainPropertiesEXT(VkDevice device, VkSwapchainKHR swapchain, VkSwapchainTimeDomainPropertiesEXT* pSwapchainTimeDomainProperties, uint64_t* pTimeDomainsCounter) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    if(native_test_find_virtual(d,swapchain))return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    PFN_vkGetSwapchainTimeDomainPropertiesEXT next = (PFN_vkGetSwapchainTimeDomainPropertiesEXT)d->gdpa(device,"vkGetSwapchainTimeDomainPropertiesEXT");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    pthread_mutex_t *lock = native_query_lock(d,swapchain);
    if (lock) pthread_mutex_lock(lock);
    VkResult result = next(device, swapchain, pSwapchainTimeDomainProperties, pTimeDomainsCounter);
    if (lock) pthread_mutex_unlock(lock);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL native_GetPastPresentationTimingEXT(VkDevice device, const VkPastPresentationTimingInfoEXT* pPastPresentationTimingInfo, VkPastPresentationTimingPropertiesEXT* pPastPresentationTimingProperties) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    if(native_test_find_virtual(d,pPastPresentationTimingInfo->swapchain))return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    PFN_vkGetPastPresentationTimingEXT next = (PFN_vkGetPastPresentationTimingEXT)d->gdpa(device,"vkGetPastPresentationTimingEXT");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    pthread_mutex_t *lock = native_query_lock(d,pPastPresentationTimingInfo->swapchain);
    if (lock) pthread_mutex_lock(lock);
    VkResult result = next(device, pPastPresentationTimingInfo, pPastPresentationTimingProperties);
    if (lock) pthread_mutex_unlock(lock);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL native_ReleaseSwapchainImagesKHR(VkDevice device, const VkReleaseSwapchainImagesInfoKHR* pReleaseInfo) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    if(native_test_find_virtual(d,pReleaseInfo->swapchain))return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    PFN_vkReleaseSwapchainImagesKHR next = (PFN_vkReleaseSwapchainImagesKHR)d->gdpa(device,"vkReleaseSwapchainImagesKHR");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    pthread_mutex_t *lock = native_query_lock(d,pReleaseInfo->swapchain);
    if (lock) pthread_mutex_lock(lock);
    VkResult result = next(device, pReleaseInfo);
    if (lock) pthread_mutex_unlock(lock);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL native_ReleaseSwapchainImagesEXT(VkDevice device, const VkReleaseSwapchainImagesInfoEXT *pReleaseInfo) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    if(native_test_find_virtual(d,pReleaseInfo->swapchain))return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    PFN_vkReleaseSwapchainImagesEXT next = (PFN_vkReleaseSwapchainImagesEXT)d->gdpa(device,"vkReleaseSwapchainImagesEXT");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    pthread_mutex_t *lock = native_query_lock(d,pReleaseInfo->swapchain);
    if (lock) pthread_mutex_lock(lock);
    VkResult result = next(device,pReleaseInfo);
    if (lock) pthread_mutex_unlock(lock);
    return result;
}

static VKAPI_ATTR void VKAPI_CALL native_SetHdrMetadataEXT(VkDevice device, uint32_t count,
        const VkSwapchainKHR *chains, const VkHdrMetadataEXT *metadata) {
    native_device *d = find_device(dispatch_key(device));
    if (!d) return;
#ifdef GFG_NATIVE_VIRTUAL_WSI
    for(uint32_t i=0;i<count;++i)if(native_test_find_virtual(d,chains[i]))return;
#endif
    PFN_vkSetHdrMetadataEXT next = (PFN_vkSetHdrMetadataEXT)d->gdpa(device,"vkSetHdrMetadataEXT");
    if (!next) return;
    /* Same list order for multi-swapchain locking; never acquire d->lock from
     * a worker while holding a WSI lock. */
    pthread_mutex_lock(&d->lock);
    for (native_swapchain *sc=d->swapchains;sc;sc=sc->next) {
        int match=0;for(uint32_t k=0;k<count;++k) if(chains[k]==sc->handle) match=1;
        pthread_mutex_t *lock=sc->output?gfg_output_wsi(sc->output):sc->ids?&sc->ids->wsi_lock:NULL;
        if(match && lock) pthread_mutex_lock(lock);
    }
    next(device,count,chains,metadata);
    for (native_swapchain *sc=d->swapchains;sc;sc=sc->next) {
        int match=0;for(uint32_t k=0;k<count;++k) if(chains[k]==sc->handle) match=1;
        pthread_mutex_t *lock=sc->output?gfg_output_wsi(sc->output):sc->ids?&sc->ids->wsi_lock:NULL;
        if(match && lock) pthread_mutex_unlock(lock);
    }
    pthread_mutex_unlock(&d->lock);
}
