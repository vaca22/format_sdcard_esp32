//
// Created by vaca on 10/20/22.
//

#include "sdcard_service.h"
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <vfs_fat_internal.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>

static const char *TAG = "sdcard_service";

#define MOUNT_POINT "/sdcard"
const char mount_point[] = MOUNT_POINT;
static sdmmc_card_t *card;


void sdcard_init() {
    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024
    };


    ESP_LOGI(TAG, "Initializing SD card");


    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.width = 4;


    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                          "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                          "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted");

    sdmmc_card_print_info(stdout, card);
}


void sdcard_deinit() {
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI(TAG, "Card unmounted");
}


static void partition_card(const esp_vfs_fat_mount_config_t *mount_config,
                           const char *drv, sdmmc_card_t *card, BYTE pdrv) {
    const size_t workbuf_size = 4096;
    void *workbuf = NULL;
    ESP_LOGW(TAG, "partitioning card");
    workbuf = ff_memalloc(workbuf_size);
    DWORD plist[] = {100, 0, 0, 0};
    f_fdisk(pdrv, plist, workbuf);
    size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(
            card->csd.sector_size,
            mount_config->allocation_unit_size);
    ESP_LOGW(TAG, "formatting card, allocation unit size=%d", alloc_unit_size);
    f_mkfs(drv, FM_ANY, alloc_unit_size, workbuf, workbuf_size);
    free(workbuf);
}

static esp_err_t mount_prepare_mem(const char *base_path,
                                   BYTE *out_pdrv,
                                   char **out_dup_path,
                                   sdmmc_card_t **out_card) {
    esp_err_t err = ESP_OK;
    char *dup_path = NULL;
    sdmmc_card_t *card = NULL;

    BYTE pdrv = FF_DRV_NOT_USED;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        ESP_LOGD(TAG, "the maximum count of volumes is already mounted");
        return ESP_ERR_NO_MEM;

    }

    card = (sdmmc_card_t *) malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        ESP_LOGD(TAG, "could not locate new sdmmc_card_t");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    dup_path = strdup(base_path);
    if (!dup_path) {
        ESP_LOGD(TAG, "could not copy base_path");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_card = card;
    *out_pdrv = pdrv;
    *out_dup_path = dup_path;
    return ESP_OK;
    cleanup:
    free(card);
    free(dup_path);
    return err;
}

static esp_err_t init_sdmmc_host(int slot, const void *slot_config, int *out_slot) {
    *out_slot = slot;
    return sdmmc_host_init_slot(slot, (const sdmmc_slot_config_t *) slot_config);
}

static void mount_to_vfs_fat(const esp_vfs_fat_mount_config_t *mount_config, sdmmc_card_t *card, uint8_t pdrv,
                             const char *base_path) {
    FATFS *fs = NULL;
    ff_diskio_register_sdmmc(pdrv, card);
    char drv[3] = {(char) ('0' + pdrv), ':', 0};
    esp_vfs_fat_register(base_path, drv, mount_config->max_files, &fs);
    f_mount(fs, drv, 1);
    partition_card(mount_config, drv, card, pdrv);
    f_mount(fs, drv, 0);
}

static void format_card(const char *base_path,
                        const sdmmc_host_t *host_config,
                        const void *slot_config,
                        const esp_vfs_fat_mount_config_t *mount_config,
                        sdmmc_card_t **out_card) {
    esp_err_t err;
    int card_handle = -1;   //uninitialized
    sdmmc_card_t *card = NULL;
    BYTE pdrv = FF_DRV_NOT_USED;
    char *dup_path = NULL;
    bool host_inited = false;
    err = mount_prepare_mem(base_path, &pdrv, &dup_path, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount_prepare failed");
        return;
    }
    err = (*host_config->init)();
    host_inited = true;
    err = init_sdmmc_host(host_config->slot, slot_config, &card_handle);
    err = sdmmc_card_init(host_config, card);
    mount_to_vfs_fat(mount_config, card, pdrv, dup_path);
    if (out_card != NULL) {
        *out_card = card;
    }
    free(dup_path);


}

static void true_format() {
    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024
    };


    ESP_LOGI(TAG, "Initializing SD card");


    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.width = 4;


    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem");
    format_card(mount_point, &host, &slot_config, &mount_config, &card);
}

void sdcard_format() {
    sdcard_deinit();
    true_format();
    ESP_LOGI("format", "complete");
}