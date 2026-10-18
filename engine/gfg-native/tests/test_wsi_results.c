/* Regression: Steam Deck AMD X11 WSI may return SUBOPTIMAL while usable. */
#include "test_wsi_results.h"
#include <stdio.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"WSI_RESULT_FAIL line=%d\n",__LINE__);return 1;} } while(0)
int main(void) {
    CHECK(gfg_test_wsi_result_usable(VK_SUCCESS));
    CHECK(gfg_test_wsi_result_usable(VK_SUBOPTIMAL_KHR));
    CHECK(!gfg_test_wsi_result_usable(VK_ERROR_OUT_OF_DATE_KHR));
    CHECK(!gfg_test_wsi_result_usable(VK_ERROR_SURFACE_LOST_KHR));
    CHECK(!gfg_test_wsi_result_usable(VK_ERROR_DEVICE_LOST));
    CHECK(gfg_test_wsi_recreate_required(VK_ERROR_OUT_OF_DATE_KHR));
    CHECK(!gfg_test_wsi_recreate_required(VK_SUBOPTIMAL_KHR));
    puts("GSF_DECK_WSI_RESULTS suboptimal_acquire_present_usable=1 outdated_recreate_required=1 PASS");
    return 0;
}
